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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 * rose-tools
 * services/plugins/resolutionSet/resolutionSet.c
 *
 * Auto-resolution plugin for vmrosd.
 *
 * Registers the tools.capability.resolution_server capability with the
 * VMX so that when the VMware window is resized, the host sends a
 * Resolution_Set TCLO command with the new width and height. We then
 * call ChangeDisplaySettings to apply the new resolution.
 *
 * Protocol constants (Resolution_Set, tools.capability.resolution_server)
 * are taken from open-vm-tools resolutionSet.c. All implementation is
 * original.
 *
 * v0.2 scope:
 *   - Advertise resolution_server capability on Capabilities_Register
 *   - Handle Resolution_Set TCLO command
 *   - Apply resolution via ChangeDisplaySettings
 *   - Report success/failure back to host via RPC reply
 *
 * TODO v0.3:
 *   - DisplayTopology_Set (multi-monitor)
 *   - DisplayTopologyModes_Set
 *   - ChangeHost3DAvailabilityHint
 *   - Config-driven enable/disable
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"
#include "roseConfig.h"

/* -----------------------------------------------------------------------
 * Protocol constants (from open-vm-tools resolutionSet.c / conf.h)
 * --------------------------------------------------------------------- */

/* TCLO command sent by VMX when the window is resized */
#define RESOLUTION_SET_CMD       "Resolution_Set"

/* -----------------------------------------------------------------------
 * ResolutionSet
 *
 * Parses "width height" from args and calls ChangeDisplaySettings.
 * Returns 1 on success, 0 on failure.
 * --------------------------------------------------------------------- */
static int
ResolutionSet(unsigned int width, unsigned int height)
{
   DEVMODEA dm;
   LONG     result;

   if (width == 0 || height == 0) {
      fprintf(stderr, "resolutionSet: invalid dimensions %ux%u\n",
              width, height);
      return 0;
   }

   ZeroMemory(&dm, sizeof dm);
   dm.dmSize       = sizeof dm;
   dm.dmPelsWidth  = width;
   dm.dmPelsHeight = height;
   dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;

   result = ChangeDisplaySettingsA(&dm, 0);

   if (result == DISP_CHANGE_SUCCESSFUL) {
      fprintf(stdout, "resolutionSet: changed to %ux%u\n", width, height);
      return 1;
   }

   fprintf(stderr, "resolutionSet: ChangeDisplaySettings(%ux%u) failed "
           "(err=%ld)\n", width, height, result);
   return 0;
}


/* -----------------------------------------------------------------------
 * ResolutionSetSendCapability
 *
 * Sends tools.capability.resolution_server to the VMX.
 * Called on ROSE_SIG_CAPABILITIES.
 * Mirrors ResolutionSetSendResolutionServerCapability in upstream.
 * --------------------------------------------------------------------- */
static void
ResolutionSetSendCapability(RoseAppCtx *ctx)
{
   char   *reply  = NULL;
   size_t  repLen = 0;

   if (ctx->rpc == NULL) {
      return;
   }

#define SEND_CAP(str) \
   RpcChannel_SendOneRaw((str), strlen(str), &reply, &repLen); \
   free(reply); reply = NULL;

   SEND_CAP("tools.capability.resolution_set 1");
   SEND_CAP("tools.capability.resolution_server " ROSE_GUEST_SERVICE " 1");
   SEND_CAP("tools.capability.display_topology_set 1");
   SEND_CAP("tools.capability.color_depth_set 1");
   SEND_CAP("tools.capability.resolution_min 0 0");
   SEND_CAP("tools.capability.unity 1");

#undef SEND_CAP
}


/* -----------------------------------------------------------------------
 * TCLO handler: Resolution_Set
 *
 * Called by the RPC channel when the VMX sends Resolution_Set.
 * Args format: "<width> <height>"
 *
 * Registered via RoseRegisterRpc in ResolutionSet_Init.
 * --------------------------------------------------------------------- */
static unsigned int
ResolutionSetTcloHandler(char const **result,
                         size_t      *resultLen,
                         const char  *name,
                         const char  *args,
                         size_t       argsSize,
                         void        *clientData)
{
   unsigned int width  = 0;
   unsigned int height = 0;
   int          ok;

   (void)name;
   (void)argsSize;
   (void)clientData;

   if (args == NULL || argsSize == 0) {
      return RpcIn_SetRetVals(result, resultLen,
                              "Invalid arguments", FALSE);
   }

   if (sscanf(args, "%u %u", &width, &height) != 2) {
      fprintf(stderr, "resolutionSet: failed to parse args: '%s'\n", args);
      return RpcIn_SetRetVals(result, resultLen,
                              "Invalid arguments", FALSE);
   }

   ok = ResolutionSet(width, height);
   return RpcIn_SetRetVals(result, resultLen, ok ? "" : "Resolution_Set failed",
                           ok ? TRUE : FALSE);
}


/* -----------------------------------------------------------------------
 * Signal handlers
 * --------------------------------------------------------------------- */

static void
ResolutionOnCapabilities(void *src, void *data)
{
   RoseAppCtx *ctx = (RoseAppCtx *)data;
   (void)src;
   ResolutionSetSendCapability(ctx);
}

static void
ResolutionOnReset(void *src, void *data)
{
   RoseAppCtx *ctx = (RoseAppCtx *)data;
   (void)src;
   /* Re-advertise capability after channel reset */
   ResolutionSetSendCapability(ctx);
}


/* -----------------------------------------------------------------------
 * Plugin callbacks
 * --------------------------------------------------------------------- */

static int
ResolutionSet_Init(RoseAppCtx *ctx)
{
   if (ctx->rpc == NULL) {
      fprintf(stderr, "resolutionSet: no RPC channel, refusing to load.\n");
      return 0;
   }

   /* Register TCLO handler for Resolution_Set */
   if (ctx->rpcIn != NULL) {
      RpcIn_RegisterCallback(ctx->rpcIn, RESOLUTION_SET_CMD,
                             ResolutionSetTcloHandler, ctx);
   }

   /* Register signal handlers */
   if (ctx->signals != NULL) {
      RoseRegisterSignal(ctx->signals, ROSE_SIG_CAPABILITIES,
                         ResolutionOnCapabilities, ctx);
      RoseRegisterSignal(ctx->signals, ROSE_SIG_RESET,
                         ResolutionOnReset, ctx);
   }

   /* Advertise capability immediately */
   ResolutionSetSendCapability(ctx);

   fprintf(stdout, "resolutionSet: initialized.\n");
   return 1;
}

static int
ResolutionSet_Tick(RoseAppCtx *ctx)
{
   /* No periodic work needed - resolution changes are event-driven */
   (void)ctx;
   return 1;
}

static void
ResolutionSet_Shutdown(RoseAppCtx *ctx)
{
   char   *reply  = NULL;
   size_t  repLen = 0;

   if (ctx->rpc == NULL) {
      return;
   }

#define SEND_CAP(str) \
   RpcChannel_SendOneRaw((str), strlen(str), &reply, &repLen); \
   free(reply); reply = NULL;

   SEND_CAP("tools.capability.resolution_set 0");
   SEND_CAP("tools.capability.resolution_server " ROSE_GUEST_SERVICE " 0");
   SEND_CAP("tools.capability.display_topology_set 0");
   SEND_CAP("tools.capability.color_depth_set 0");
   SEND_CAP("tools.capability.unity 0");

#undef SEND_CAP
}


/* -----------------------------------------------------------------------
 * DllMain
 * --------------------------------------------------------------------- */
BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
   (void)hinstDLL;
   (void)fdwReason;
   (void)lpvReserved;
   return TRUE;
}


/* -----------------------------------------------------------------------
 * RoseOnLoad
 * --------------------------------------------------------------------- */
ROSE_MODULE_EXPORT RosePluginData *
RoseOnLoad(RoseAppCtx *ctx)
{
   static RosePluginData regData;

   if (ctx->rpc == NULL) {
      return NULL;
   }

   /* Disabled by default - requires VMware SVGA driver for arbitrary
    * resolution support. Enable via [resolutionset] enable=true in
    * tools.conf once the SVGA driver is installed. */
   if (!RoseConfig_GetBool(ctx->config, "resolutionset", "enable", 0)) {
      fprintf(stdout, "resolutionSet: Disabled. See documentation to learn more.\n");
      return NULL;
   }

   memset(&regData, 0, sizeof regData);
   regData.name           = "resolutionSet";
   regData.init           = ResolutionSet_Init;
   regData.tick           = ResolutionSet_Tick;
   regData.shutdown       = ResolutionSet_Shutdown;
   regData.tickIntervalMs = 0;   /* event-driven, no tick needed */

   return &regData;
}
