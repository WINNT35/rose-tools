/*********************************************************
 * Copyright (c) 2008-2024 Broadcom. All Rights Reserved.
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 * Copyright (C) 2026 WINNT35
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "vm_tools_version.h"
#include "vm_version.h"
#include "rpcin.h"
#include "message.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"

/* Whether we have already sent the version to the VMX log */
static int g_versionSent   = 0;
static int g_pendingReset  = 0;  /* set by reset handler, cleared by Poll after ATR is sent */

/* Application context pointer - set during init for use in callbacks */
static RoseAppCtx *g_ctx = NULL;

/* Selftest flag setter - implemented in mainLoop.c */
extern void RoseSelfTest_SetFlag(int flag);
#define ST_RESET_OK  (1 << 4)
#define ST_CAPS_OK   (1 << 5)

/* Forward declarations */
static unsigned int ToolsCoreRpcReset(char const **result, size_t *resultLen,
                                      const char *name, const char *args,
                                      size_t argsSize, void *clientData);
static unsigned int ToolsCoreRpcCapReg(char const **result, size_t *resultLen,
                                       const char *name, const char *args,
                                       size_t argsSize, void *clientData);
static unsigned int ToolsCoreRpcSetOption(char const **result, size_t *resultLen,
                                          const char *name, const char *args,
                                          size_t argsSize, void *clientData);


/* -----------------------------------------------------------------------
 * RpcIn error callback
 *
 * Called by rpcin.c when the TCLO channel encounters an error.
 * Mirrors RpcChannelError in rpcChannel.c: sets errorCode and stops
 * the main loop so vmrosd can exit cleanly and be restarted.
 * --------------------------------------------------------------------- */
static void
ToolsCoreRpcError(void *data, char const *errmsg)
{
   (void)data;
   fprintf(stderr, "toolsRpc: inbound RPC error: %s\n",
           errmsg ? errmsg : "(null)");
   if (g_ctx != NULL) {
      ROSEAPP_ERROR(g_ctx, 1);
   }
}


/* -----------------------------------------------------------------------
 * ToolsCoreCheckReset
 *
 * Called from ToolsCoreRpcReset after the "ATR" reply goes out.
 * Advertises unified loop support, logs version to VMX once.
 * Mirrors ToolsCoreCheckReset in open-vm-tools toolsRpc.c.
 * --------------------------------------------------------------------- */
static void
ToolsCoreCheckReset(void)
{
   char   buf[256];
   char  *reply  = NULL;
   size_t repLen = 0;

   /*
    * Advertise unified loop support. Without this VMware will not send
    * Capabilities_Register or Set_Option over TCLO.
    * Must be sent on every reset. Mirrors the first thing
    * ToolsCoreCheckReset does in upstream.
    */
   _snprintf(buf, sizeof buf,
             "vmx.capability.unified_loop %s", ROSE_GUEST_SERVICE);
   buf[sizeof buf - 1] = '\0';
   if (!RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen)) {
      fprintf(stderr, "toolsRpc: VMX does not support unified_loop\n");
   }
   free(reply); reply = NULL;
   RoseSelfTest_SetFlag(ST_RESET_OK);

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
}


/* -----------------------------------------------------------------------
 * ToolsCoreRpcReset
 *
 * Handler for the "reset" TCLO command from the host.
 * Registered with RpcIn_RegisterCallback (old-style RpcIn_OldCallback).
 * Must reply "ATR <appname>" to tell VMware which application we are.
 * Mirrors RpcChannelReset in rpcChannel.c.
 * --------------------------------------------------------------------- */
static unsigned int
ToolsCoreRpcReset(char const **result,
                  size_t      *resultLen,
                  const char  *name,
                  const char  *args,
                  size_t       argsSize,
                  void        *clientData)
{
   static char reply[64];

   (void)name;
   (void)args;
   (void)argsSize;
   (void)clientData;

   _snprintf(reply, sizeof reply, "ATR %s", ROSE_GUEST_SERVICE);
   reply[sizeof reply - 1] = '\0';
   reply[sizeof reply - 1] = '\0';

   /* ROSE-TOOLS BEGIN: deferred post-reset work.
    * unified_loop and version log must be sent AFTER ATR is delivered,
    * not during the inbound handler. Set a flag; Poll() will pick it up
    * on the next tick once ATR has been sent back to VMware.
    * ROSE-TOOLS ORIGINAL: ToolsCoreCheckReset() called inline here. */
   g_pendingReset = 1;
   /* ROSE-TOOLS END */

   return RpcIn_SetRetVals(result, resultLen, reply, TRUE);
}


/* -----------------------------------------------------------------------
 * ToolsCoreRpcCapReg
 *
 * Handler for "Capabilities_Register" TCLO command from host.
 * Registered with RpcIn_RegisterCallback (old-style callback).
 * Sends tools.set.version* and emits ROSE_SIG_CAPABILITIES.
 * Mirrors ToolsCoreRpcCapReg in open-vm-tools toolsRpc.c.
 * --------------------------------------------------------------------- */
static unsigned int
ToolsCoreRpcCapReg(char const **result,
                   size_t      *resultLen,
                   const char  *name,
                   const char  *args,
                   size_t       argsSize,
                   void        *clientData)
{
   char   buf[256];
   char  *reply  = NULL;
   size_t repLen = 0;
   unsigned int ok = TRUE;

   (void)name;
   (void)args;
   (void)argsSize;
   (void)clientData;

   RoseSelfTest_SetFlag(ST_CAPS_OK);

   /* Notify plugins to register their capabilities */
   if (g_ctx != NULL && g_ctx->signals != NULL) {
      RoseEmitSignal(g_ctx->signals, ROSE_SIG_CAPABILITIES, g_ctx);
   }

   /* TODO v0.3: send tools.capability.guest_conf_directory */

   /*
    * Send tools version to host. Try tools.set.versiontype first
    * (includes install type); fall back to tools.set.version for
    * older hosts. Always TOOLS_TYPE_MSI on Windows.
    */
   _snprintf(buf, sizeof buf,
             "tools.set.versiontype %u %u",
             (unsigned)TOOLS_VERSION_CURRENT,
             (unsigned)TOOLS_TYPE_MSI);
   buf[sizeof buf - 1] = '\0';

   if (!RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen)) {
      free(reply); reply = NULL;

      _snprintf(buf, sizeof buf,
                "tools.set.version %u",
                (unsigned)TOOLS_VERSION_CURRENT);
      buf[sizeof buf - 1] = '\0';

      if (!RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen)) {
         ok = FALSE;
      }
   }
   free(reply);

   return RpcIn_SetRetVals(result, resultLen, "", ok);
}


/* -----------------------------------------------------------------------
 * ToolsCoreRpcSetOption
 *
 * Handler for "Set_Option" TCLO command from host.
 * Registered with RpcIn_RegisterCallback (old-style callback).
 * Parses "optionName value" and emits ROSE_SIG_SET_OPTION.
 * Mirrors ToolsCoreRpcSetOption in open-vm-tools toolsRpc.c.
 * --------------------------------------------------------------------- */
static unsigned int
ToolsCoreRpcSetOption(char const **result,
                      size_t      *resultLen,
                      const char  *name,
                      const char  *args,
                      size_t       argsSize,
                      void        *clientData)
{
   char *buf;
   char *option;
   char *value;
   char *sep;

   (void)name;
   (void)clientData;

   if (args == NULL || argsSize == 0) {
      return RpcIn_SetRetVals(result, resultLen,
                              "Unknown or invalid option", FALSE);
   }

   buf = (char *)malloc(argsSize + 1);
   if (buf == NULL) {
      return RpcIn_SetRetVals(result, resultLen, "out of memory", FALSE);
   }
   memcpy(buf, args, argsSize);
   buf[argsSize] = '\0';

   sep = strchr(buf, ' ');
   if (sep != NULL) {
      *sep   = '\0';
      option = buf;
      value  = sep + 1;
   } else {
      option = buf;
      value  = "";
   }

   if (option[0] == '\0') {
      free(buf);
      return RpcIn_SetRetVals(result, resultLen,
                              "Unknown or invalid option", FALSE);
   }

   /* TODO v0.3: pass option+value to signal handlers */
   (void)value;

   if (g_ctx != NULL && g_ctx->signals != NULL) {
      RoseEmitSignal(g_ctx->signals, ROSE_SIG_SET_OPTION, g_ctx);
   }

   free(buf);
   return RpcIn_SetRetVals(result, resultLen, "", TRUE);
}


/* -----------------------------------------------------------------------
 * RoseCore_SetCapabilities
 *
 * Transmits a capability array to the host.
 * Mirrors ToolsCore_SetCapabilities.
 * TODO v0.3: TOOLS_CAP_OLD_NOVAL and TOOLS_CAP_NEW.
 * --------------------------------------------------------------------- */
void
RoseCore_SetCapabilities(RoseAppCtx *ctx,
                         RoseAppCapability *caps,
                         int capCount,
                         int set)
{
   int    i;
   char   buf[256];
   char  *reply  = NULL;
   size_t repLen = 0;

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
         /* TODO v0.3 */
         break;
      case ROSE_CAP_NEW:
         /* TODO v0.3 */
         break;
      default:
         fprintf(stderr, "toolsRpc: unknown capability type %d\n",
                 caps[i].type);
         break;
      }
   }
}


/* -----------------------------------------------------------------------
 * RoseToolsRpc_Init
 *
 * Initializes both outbound (RpcChannel) and inbound (RpcIn) channels.
 * Mirrors ToolsCore_InitRpc.
 * Returns non-zero on success.
 * --------------------------------------------------------------------- */
int
RoseToolsRpc_Init(RoseAppCtx *ctx)
{
   if (ctx == NULL) {
      return 0;
   }

   g_ctx         = ctx;
   g_versionSent = 0;

   if (!ctx->isVMware) {
      ctx->rpc   = NULL;
      ctx->rpcIn = NULL;
      return 1;
   }

   /* --- Outbound channel (RpcChannel / backdoor) --- */
   ctx->rpc = RpcChannel_New();
   if (ctx->rpc == NULL) {
      fprintf(stderr, "toolsRpc: failed to create outbound RPC channel\n");
      return 0;
   }

   if (!RpcChannel_Start(ctx->rpc)) {
      fprintf(stderr, "toolsRpc: failed to start outbound RPC channel\n");
      RpcChannel_Destroy(ctx->rpc);
      ctx->rpc = NULL;
      return 0;
   }

   /* --- Inbound channel (RpcIn / TCLO) --- */
   ctx->rpcIn = RpcIn_Construct(&gRoseTimerQueueStorage);
   if (ctx->rpcIn == NULL) {
      fprintf(stderr, "toolsRpc: failed to construct inbound RPC channel\n");
      RpcChannel_Stop(ctx->rpc);
      RpcChannel_Destroy(ctx->rpc);
      ctx->rpc = NULL;
      return 0;
   }

   /*
    * Register built-in TCLO handlers.
    * Note: "ping" is registered automatically by RpcIn_start.
    * "reset" is passed as resetCallback to RpcIn_start.
    * We register Capabilities_Register and Set_Option explicitly.
    */
   RpcIn_RegisterCallback(ctx->rpcIn, "Capabilities_Register",
                          ToolsCoreRpcCapReg, ctx);
   RpcIn_RegisterCallback(ctx->rpcIn, "Set_Option",
                          ToolsCoreRpcSetOption, ctx);

   /*
    * Start the inbound channel. This opens the TCLO backdoor channel
    * and registers "reset" and "ping". RPCIN_MAX_DELAY_CS is the
    * maximum poll backoff in centiseconds (defined in rpcin.h).
    */
   if (!RpcIn_start(ctx->rpcIn,
                    RPCIN_MAX_DELAY_CS,
                    ToolsCoreRpcReset,  /* reset callback */
                    ctx,                /* reset client data */
                    ToolsCoreRpcError,  /* error callback */
                    NULL,               /* clearError callback */
                    ctx)) {
      fprintf(stderr,
              "toolsRpc: failed to start inbound RPC channel.\n"
              "toolsRpc: Check that you are running inside a "
              "VMware guest.\n");
      RpcIn_Destruct(ctx->rpcIn);
      ctx->rpcIn = NULL;
      RpcChannel_Stop(ctx->rpc);
      RpcChannel_Destroy(ctx->rpc);
      ctx->rpc = NULL;
      return 0;
   }

   return 1;
}


/* -----------------------------------------------------------------------
 * RoseToolsRpc_Poll
 *
 * Drives one iteration of the inbound TCLO receive loop.
 * Must be called from the main loop on every tick.
 * Returns non-zero if the channel is healthy, zero on error.
 * --------------------------------------------------------------------- */
int
RoseToolsRpc_Poll(RoseAppCtx *ctx)
{
   if (ctx == NULL || ctx->rpcIn == NULL) {
      return 1;   /* not started - not an error */
   }
   {
      int ok = (int)RpcIn_Poll(ctx->rpcIn);
      /* Handle deferred post-reset work AFTER RpcIn_Poll returns.
       * This ensures the "OK ATR vmtoolsd" was delivered by RpcInSend
       * before we send vmx.capability.unified_loop on the RPCI channel.
       * VMware must see the ATR on TCLO before it will process unified_loop. */
      if (g_pendingReset) {
         g_pendingReset = 0;
         ToolsCoreCheckReset();
      }
      return ok;
   }
}


/* -----------------------------------------------------------------------
 * RoseToolsRpc_Shutdown
 *
 * Stops and destroys both inbound and outbound channels.
 * --------------------------------------------------------------------- */
void
RoseToolsRpc_Shutdown(RoseAppCtx *ctx)
{
   if (ctx == NULL) {
      return;
   }

   if (ctx->rpcIn != NULL) {
      RpcIn_stop(ctx->rpcIn);
      RpcIn_Destruct(ctx->rpcIn);
      ctx->rpcIn = NULL;
   }

   if (ctx->rpc != NULL) {
      RpcChannel_Stop(ctx->rpc);
      RpcChannel_Destroy(ctx->rpc);
      ctx->rpc = NULL;
   }

   g_ctx = NULL;
}
