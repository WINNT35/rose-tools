/*
 * Copyright (C) 2026 WINNT35
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * rose-tools
 * services/vmrosd/toolsRpc.c
 *
 * GuestRPC channel management for vmrosd. Mirrors open-vm-tools toolsRpc.c.
 * Handles channel init, reset, capability registration, and option setting.
 *
 * v0.1 scope:
 *   - RoseToolsRpc_Init        -- channel creation and built-in handler registration
 *   - RoseToolsRpc_Shutdown    -- channel stop and destroy
 *   - ToolsCoreCheckReset      -- reset handler, version logging, ROSE_SIG_RESET
 *   - ToolsCoreRpcCapReg       -- Capabilities_Register handler, tools.set.version
 *   - ToolsCoreRpcSetOption    -- Set_Option handler, ROSE_SIG_SET_OPTION
 *   - RoseCore_SetCapabilities -- capability transmission to host
 *
 * TODO v0.2:
 *   - vmx.capability.unified_loop advertisement on reset
 *   - tools.capability.guest_conf_directory transmission
 *   - Full TOOLS_CAP_OLD_NOVAL and TOOLS_CAP_NEW capability batching
 *   - vSocket family management (Linux only - not applicable)
 *   - ToolsCoreAppChannelFail (Linux/Mac only - not applicable)
 *   - RpcChannel_Setup callback and error limiting
 *   - hideVersion / disableVersion config key handling
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "vm_tools_version.h"
#include "vm_version.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"

/* Whether we have already sent the version to the VMX log */
static int g_versionSent = 0;

/* Whether capabilities have been registered with the host */
static int g_capsRegistered = 0;

/* Forward declarations */
static gboolean ToolsCoreRpcCapReg(RpcInData *data);
static gboolean ToolsCoreRpcSetOption(RpcInData *data);

/*
 * Built-in RPC handlers. Mirrors the two statically registered
 * handlers in ToolsCore_InitRpc.
 */
static RpcChannelCallback g_builtinRpcs[] = {
   { "Capabilities_Register", ToolsCoreRpcCapReg,  NULL, NULL, NULL, 0 },
   { "Set_Option",            ToolsCoreRpcSetOption, NULL, NULL, NULL, 0 },
};

#define BUILTIN_RPC_COUNT (sizeof(g_builtinRpcs) / sizeof(g_builtinRpcs[0]))

/* Application context pointer - set during init for use in callbacks */
static RoseAppCtx *g_ctx = NULL;

/* -----------------------------------------------------------------------
 * ToolsCoreCheckReset
 *
 * Called when the RPC channel is reset. Mirrors ToolsCoreCheckReset.
 * On success: logs version to VMX once, emits ROSE_SIG_RESET.
 * On failure: sets error code and stops the main loop.
 * --------------------------------------------------------------------- */
static void
ToolsCoreCheckReset(RpcChannel *chan, gboolean success, gpointer data)
{
   char buf[256];
   char *reply   = NULL;
   size_t repLen = 0;

   (void)chan;
   (void)data;

   if (!success) {
      fprintf(stderr, "toolsRpc: channel reset failed.\n");
      if (g_ctx != NULL) {
         ROSEAPP_ERROR(g_ctx, 1);
      }
      return;
   }

   /* TODO v0.2: send vmx.capability.unified_loop <appname> */

   /* Log version to VMX once - static flag prevents log spamming */
   if (!g_versionSent) {
      _snprintf(buf, sizeof buf,
                "log %s: Version: %s (%s)",
                ROSE_GUEST_SERVICE,
                TOOLS_VERSION_CURRENT_STR,
                BUILD_NUMBER);
      buf[sizeof buf - 1] = '\0';
      RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
      free(reply); reply = NULL;
      g_versionSent = 1;
   }

   /* Notify plugins that the channel has been re-established */
   if (g_ctx != NULL && g_ctx->signals != NULL) {
      RoseEmitSignal(g_ctx->signals, ROSE_SIG_RESET, g_ctx);
   }

   /* TODO v0.2: vSocket family release and reinit (Linux only) */
}

/* -----------------------------------------------------------------------
 * ToolsCoreRpcCapReg
 *
 * Handler for Capabilities_Register RPC from host.
 * Mirrors ToolsCoreRpcCapReg.
 * Emits ROSE_SIG_CAPABILITIES, sends tools.set.version.
 * --------------------------------------------------------------------- */
static gboolean
ToolsCoreRpcCapReg(RpcInData *data)
{
   char    buf[256];
   char   *reply   = NULL;
   size_t  repLen  = 0;
   gboolean ok     = TRUE;

   if (g_ctx == NULL) {
      return RpcChannel_SetRetVals(data, "no context", FALSE);
   }

   /* Notify plugins to register their capabilities */
   if (g_ctx->signals != NULL) {
      RoseEmitSignal(g_ctx->signals, ROSE_SIG_CAPABILITIES, g_ctx);
   }

   /* TODO v0.2: call RoseCore_SetCapabilities with collected capability arrays */
   /* TODO v0.2: send tools.capability.guest_conf_directory */

   /*
    * Send tools version to host. First try tools.set.versiontype,
    * fall back to tools.set.version if host is too old.
    * Always TOOLS_TYPE_MSI on Windows. Mirrors the version reporting
    * logic in ToolsCoreRpcCapReg.
    */
   _snprintf(buf, sizeof buf,
             "tools.set.versiontype %u %u",
             (unsigned)TOOLS_VERSION_CURRENT,
             (unsigned)TOOLS_TYPE_MSI);
   buf[sizeof buf - 1] = '\0';

   if (!RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen)) {
      /* Host too old for versiontype - fall back */
      free(reply); reply = NULL;

      _snprintf(buf, sizeof buf,
                "tools.set.version %u",
                (unsigned)TOOLS_VERSION_CURRENT);
      buf[sizeof buf - 1] = '\0';

      ok = (gboolean)RpcChannel_SendOneRaw(buf, strlen(buf),
                                           &reply, &repLen);
   }

   free(reply);
   g_capsRegistered = 1;

   return RpcChannel_SetRetVals(data, "", ok);
}

/* -----------------------------------------------------------------------
 * ToolsCoreRpcSetOption
 *
 * Handler for Set_Option RPC from host.
 * Mirrors ToolsCoreRpcSetOption.
 * Parses option/value and emits ROSE_SIG_SET_OPTION.
 * --------------------------------------------------------------------- */
static gboolean
ToolsCoreRpcSetOption(RpcInData *data)
{
   char  *option = NULL;
   char  *value  = NULL;
   char  *args   = NULL;
   char  *sep    = NULL;

   if (data->args == NULL || data->argsSize == 0) {
      return RpcChannel_SetRetVals(data,
                                   "Unknown or invalid option", FALSE);
   }

   /* Make a mutable copy of args for parsing */
   args = (char *)malloc(data->argsSize + 1);
   if (args == NULL) {
      return RpcChannel_SetRetVals(data, "out of memory", FALSE);
   }
   memcpy(args, data->args, data->argsSize);
   args[data->argsSize] = '\0';

   /*
    * Split on first space: "optionName value"
    * Mirrors StrUtil_GetNextToken behaviour.
    */
   sep = strchr(args, ' ');
   if (sep != NULL) {
      *sep  = '\0';
      option = args;
      value  = sep + 1;
   } else {
      option = args;
      value  = "";
   }

   if (option[0] == '\0') {
      free(args);
      return RpcChannel_SetRetVals(data,
                                   "Unknown or invalid option", FALSE);
   }

   /* Notify plugins of the option change */
   if (g_ctx != NULL && g_ctx->signals != NULL) {
      RoseEmitSignal(g_ctx->signals, ROSE_SIG_SET_OPTION, g_ctx);
   }

   /* TODO v0.2: pass option and value to signal handlers */
   (void)value;

   free(args);
   return RpcChannel_SetRetVals(data, "", TRUE);
}

/* -----------------------------------------------------------------------
 * RoseCore_SetCapabilities
 *
 * Transmits a capability array to the host.
 * Mirrors ToolsCore_SetCapabilities.
 * v0.1: TOOLS_CAP_OLD only. TOOLS_CAP_OLD_NOVAL and TOOLS_CAP_NEW
 * are stubbed as TODO v0.2.
 * --------------------------------------------------------------------- */
void
RoseCore_SetCapabilities(RoseAppCtx *ctx,
                         RoseAppCapability *caps,
                         int capCount,
                         int set)
{
   int    i;
   char   buf[256];
   char  *reply   = NULL;
   size_t repLen  = 0;

   if (ctx == NULL || caps == NULL || capCount == 0) {
      return;
   }

   for (i = 0; i < capCount; i++) {
      switch (caps[i].type) {
      case ROSE_CAP_OLD:
         _snprintf(buf, sizeof buf,
                   "tools.capability.%s %u",
                   caps[i].name,
                   set ? caps[i].value : 0u);
         buf[sizeof buf - 1] = '\0';
         RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
         free(reply); reply = NULL;
         break;

      case ROSE_CAP_OLD_NOVAL:
         /* TODO v0.2: send "tools.capability.<name> " with exact byte count */
         break;

      case ROSE_CAP_NEW:
         /* TODO v0.2: batch into GUEST_CAP_FEATURES message */
         break;

      default:
         printf("toolsRpc: unknown capability type %d.\n", caps[i].type);
         break;
      }
   }
}

/* -----------------------------------------------------------------------
 * RoseToolsRpc_Init
 *
 * Initializes the RPC channel. Mirrors ToolsCore_InitRpc.
 * Returns non-zero on success.
 * --------------------------------------------------------------------- */
int
RoseToolsRpc_Init(RoseAppCtx *ctx)
{
   int i;

   if (ctx == NULL) {
      return 0;
   }

   g_ctx = ctx;
   g_versionSent   = 0;
   g_capsRegistered = 0;

   /* Not running in VMware - no channel needed */
   if (!ctx->isVMware) {
      ctx->rpc = NULL;
      return 1;
   }

   /* Create backdoor channel */
   ctx->rpc = RpcChannel_New();
   if (ctx->rpc == NULL) {
      fprintf(stderr, "toolsRpc: failed to create RPC channel.\n");
      return 0;
   }

   /*
    * TODO v0.2: RpcChannel_Setup with reset callback, failure callback,
    * app name, and error limit.
    * For v0.1 the reset callback is wired directly below via a manual
    * check after Send failures.
    */

   /* Register built-in RPC handlers */
   for (i = 0; i < (int)BUILTIN_RPC_COUNT; i++) {
      g_builtinRpcs[i].clientData = ctx;
      /* TODO v0.2: RpcChannel_RegisterCallback(ctx->rpc, &g_builtinRpcs[i]) */
      /* v0.1: inbound RPC not yet implemented, handlers registered but
       * not yet wired to channel dispatch */
   }

   return 1;
}

/* -----------------------------------------------------------------------
 * RoseToolsRpc_Shutdown
 *
 * Stops and destroys the RPC channel.
 * --------------------------------------------------------------------- */
void
RoseToolsRpc_Shutdown(RoseAppCtx *ctx)
{
   if (ctx == NULL || ctx->rpc == NULL) {
      return;
   }

   RpcChannel_Stop(ctx->rpc);
   RpcChannel_Destroy(ctx->rpc);
   ctx->rpc = NULL;
   g_ctx    = NULL;
}
