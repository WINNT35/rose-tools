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
 * services/vmrosd/mainLoop.c
 *
 * Core service lifecycle for vmrosd. Mirrors open-vm-tools mainLoop.c.
 * Handles setup, RPC channel init, version reporting, plugin loading,
 * main loop execution, and teardown.
 *
 * v0.1 scope:
 *   - RoseCore_Setup    -- context init, VMware check
 *   - RoseCore_Run      -- RPC init, version report, plugin load, loop
 *   - RoseCore_Cleanup  -- plugin shutdown, RPC teardown
 *
 * TODO v0.2:
 *   - Thread pool (ToolsCorePool_Init equivalent)
 *   - GuestStore client
 *   - GlobalConfig module
 *   - vSocket family management
 *   - Hang detector
 *   - Environment variable management (ToolsCoreInitEnv equivalent)
 *   - Debug plugin loading
 *   - vmusr channel error limiting
 *   - Config poll timer (RoseCore_ReloadConfig on interval)
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
#include "vmcheck.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"

/* Forward declarations for pluginMgr.c and toolsRpc.c */
static void RoseCore_Cleanup(RoseAppCtx *ctx);
extern int  RosePluginMgr_Load(RoseAppCtx *ctx);
extern void RosePluginMgr_Tick(RoseAppCtx *ctx);
extern void RosePluginMgr_Shutdown(RoseAppCtx *ctx);
extern int  RoseToolsRpc_Init(RoseAppCtx *ctx);
extern void RoseToolsRpc_Shutdown(RoseAppCtx *ctx);

/* Main loop sleep interval in milliseconds */
#define MAIN_LOOP_SLEEP_MS  100

/* Config poll interval in milliseconds (mirrors CONF_POLL_TIME * 1000) */
#define CONF_POLL_MS        (60 * 1000)

/* -----------------------------------------------------------------------
 * ToolsCoreReportVersionData equivalent
 *
 * Sends vmtools version data to the host as guestinfo variables.
 * Mirrors the four info-set calls in ToolsCoreReportVersionData.
 * --------------------------------------------------------------------- */
static void
RoseCoreReportVersionData(RoseAppCtx *ctx)
{
   char buf[256];
   char *reply   = NULL;
   size_t repLen = 0;

   if (ctx->rpc == NULL) {
      return;
   }

   /* guestinfo.vmtools.description */
   _snprintf(buf, sizeof buf,
             "info-set guestinfo.vmtools.description"
             " VMware rose-tools %s",
             TOOLS_VERSION_CURRENT_STR);
   buf[sizeof buf - 1] = '\0';
   RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
   free(reply); reply = NULL;

   /* guestinfo.vmtools.versionString */
   _snprintf(buf, sizeof buf,
             "info-set guestinfo.vmtools.versionString %s",
             TOOLS_VERSION_CURRENT_STR);
   buf[sizeof buf - 1] = '\0';
   RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
   free(reply); reply = NULL;

   /* guestinfo.vmtools.versionNumber */
   _snprintf(buf, sizeof buf,
             "info-set guestinfo.vmtools.versionNumber %u",
             (unsigned)TOOLS_VERSION_CURRENT);
   buf[sizeof buf - 1] = '\0';
   RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
   free(reply); reply = NULL;

   /* guestinfo.vmtools.buildNumber */
   _snprintf(buf, sizeof buf,
             "info-set guestinfo.vmtools.buildNumber %s",
             BUILD_NUMBER);
   buf[sizeof buf - 1] = '\0';
   RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
   free(reply); reply = NULL;
}

/* -----------------------------------------------------------------------
 * RoseCore_Setup
 *
 * Initializes the application context before the main loop runs.
 * Mirrors ToolsCore_Setup.
 * --------------------------------------------------------------------- */
int
RoseCore_Setup(RoseAppCtx *ctx)
{
   if (ctx == NULL) {
      return 0;
   }

   /* Check if running under VMware */
   ctx->isVMware = VmCheck_IsVirtualWorld();
   if (!ctx->isVMware) {
      fprintf(stderr, "vmrosd: not running in a VMware VM, exiting.\n");
      return 0;
   }

   /* TODO v0.2: ToolsCoreInitEnv equivalent (env var management) */
   /* TODO v0.2: ToolsCorePool_Init equivalent (thread pool) */
   /* TODO v0.2: debug plugin loading */

   return 1;
}

/* -----------------------------------------------------------------------
 * RoseCore_Run
 *
 * Main execution function. Mirrors ToolsCoreRunLoop.
 * Inits RPC, reports version, loads plugins, runs loop, cleans up.
 * --------------------------------------------------------------------- */
void
RoseCore_Run(RoseAppCtx *ctx)
{
   DWORD    now;
   DWORD    lastConfPoll;
   DWORD    lastHeartbeat;
   int      pluginsLoaded;

   if (ctx == NULL) {
      return;
   }

   /* TODO v0.2: VSockets init (Windows) */

   /* Init RPC channel */
   if (!RoseToolsRpc_Init(ctx)) {
      fprintf(stderr, "vmrosd: RPC channel init failed.\n");
      ROSEAPP_ERROR(ctx, 1);
      goto cleanup;
   }

   /* Start RPC channel */
   if (!RpcChannel_Start(ctx->rpc)) {
      fprintf(stderr, "vmrosd: failed to start RPC channel.\n");
      ROSEAPP_ERROR(ctx, 1);
      goto cleanup;
   }

   /* Report version data to VMX - this is the "tools running" signal */
   RoseCoreReportVersionData(ctx);

   /* TODO v0.2: tools notifications (Windows) */
   /* TODO v0.2: GuestStore client init */

   /* Load plugins */
   pluginsLoaded = RosePluginMgr_Load(ctx);
   if (pluginsLoaded == 0) {
      fprintf(stderr, "vmrosd: no plugins loaded.\n");
      ROSEAPP_ERROR(ctx, 1);
      goto cleanup;
   }

   printf("vmrosd: %d plugin(s) loaded, entering main loop.\n",
          pluginsLoaded);

   /* TODO v0.2: vSocket family reference (Linux main service) */
   /* TODO v0.2: hang detector */
   /* TODO v0.2: GlobalConfig module */

   /* Connect core signals */
   /* TODO v0.2: ROSE_SIG_IO_FREEZE handler (config poll suspend/resume) */
   /* TODO v0.2: ROSE_SIG_SET_OPTION handler (log level changes) */
   /* TODO v0.2: ROSE_SIG_RESET handler (VMX guest logger reinit) */

   lastConfPoll  = GetTickCount();
   lastHeartbeat = GetTickCount();

   /* Main loop - mirrors g_main_loop_run */
   while (ctx->running && ctx->errorCode == 0) {
      now = GetTickCount();

      /* Periodic heartbeat - resend tools version every 10s.
       * VMX times out tools as inactive after ~20s without a heartbeat. */
      if ((now - lastHeartbeat) >= 10000u) {
         RoseCoreReportVersionData(ctx);
         lastHeartbeat = now;
      }

      /* Tick plugins */
      RosePluginMgr_Tick(ctx);

      /* Periodic config reload */
      if ((now - lastConfPoll) >= (DWORD)CONF_POLL_MS) {
         /* TODO v0.2: RoseCore_ReloadConfig(ctx) */
         lastConfPoll = now;
      }

      Sleep(MAIN_LOOP_SLEEP_MS);
   }

cleanup:
   RoseCore_Cleanup(ctx);
}

/* -----------------------------------------------------------------------
 * RoseCore_Cleanup
 *
 * Tears down the service in reverse order. Mirrors ToolsCoreCleanup.
 * --------------------------------------------------------------------- */
void
RoseCore_Cleanup(RoseAppCtx *ctx)
{
   if (ctx == NULL) {
      return;
   }

   /* Emit pre-shutdown signal */
   if (ctx->signals != NULL) {
      RoseEmitSignal(ctx->signals, ROSE_SIG_PRE_SHUTDOWN, ctx);
   }

   /* TODO v0.2: GuestStore plugin shutdown first (deadlock prevention) */
   /* TODO v0.2: thread pool shutdown */

   /* Unload plugins */
   RosePluginMgr_Shutdown(ctx);

   /* Emit shutdown signal */
   if (ctx->signals != NULL) {
      RoseEmitSignal(ctx->signals, ROSE_SIG_SHUTDOWN, ctx);
   }

   /* TODO v0.2: vSocket family release */
   /* TODO v0.2: GuestStore client deinit */
   /* TODO v0.2: tools notifications end (Windows) */

   /* Shutdown RPC */
   RoseToolsRpc_Shutdown(ctx);

   /* TODO v0.2: free config */
   /* TODO v0.2: uninitialize COM */

   printf("vmrosd: cleanup complete.\n");
}
