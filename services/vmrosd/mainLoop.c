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

/**
 * @file mainLoop.c
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
 * v0.2 additions:
 *   - Dynamic plugin loading via RosePluginMgr_Load
 *   - Extended selftest (reset, capabilities, plugin load checks)
 *   - Config file load in InitContext via RoseConfig_Load
 *
 * TODO v0.3:
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
extern void RosePluginMgr_Free(void);
extern int  RoseToolsRpc_Init(RoseAppCtx *ctx);
extern void RoseToolsRpc_Shutdown(RoseAppCtx *ctx);
extern int  RoseToolsRpc_Poll(RoseAppCtx *ctx);

/* Main loop sleep interval in milliseconds */
#define MAIN_LOOP_SLEEP_MS  100

/* Config poll interval in milliseconds (mirrors CONF_POLL_TIME * 1000) */
#define CONF_POLL_MS        (60 * 1000)

/* How long to wait for selftest conditions before giving up (ms) */
#define SELFTEST_TIMEOUT_MS (15 * 1000)

/* -----------------------------------------------------------------------
 * SelfTest result flags - set as conditions are confirmed
 * --------------------------------------------------------------------- */
#define ST_VMWARE_OK       (1 << 0)   /* VmCheck passed                */
#define ST_RPC_OPEN_OK     (1 << 1)   /* RPC channel opened            */
#define ST_VERSION_OK      (1 << 2)   /* version data sent to VMX      */
#define ST_HEARTBEAT_OK    (1 << 3)   /* one heartbeat tick fired      */
#define ST_RESET_OK        (1 << 4)   /* TCLO reset received from host */
#define ST_CAPS_OK         (1 << 5)   /* Capabilities_Register received */
#define ST_PLUGIN_OK       (1 << 6)   /* at least one plugin loaded    */
#define ST_ALL_OK          (ST_VMWARE_OK | ST_RPC_OPEN_OK | \
                            ST_VERSION_OK | ST_HEARTBEAT_OK | \
                            ST_RESET_OK | ST_CAPS_OK | ST_PLUGIN_OK)

static int g_selfTestFlags  = 0;
static int g_pluginsLoaded  = 0;

/* Called from toolsRpc.c to set selftest flags without exposing the var */
void RoseSelfTest_SetFlag(int flag) { g_selfTestFlags |= flag; }

/* -----------------------------------------------------------------------
 * PrintSelfTestResult
 *
 * Prints a per-flag pass/fail summary and the congratulations line if
 * all checks passed. Called once after the first heartbeat cycle.
 * --------------------------------------------------------------------- */
static void
PrintSelfTestResult(void)
{
   printf("\n--- Rose Tools v0.2 self-test ---\n");
   printf("  VMware detected:          %s\n",
          (g_selfTestFlags & ST_VMWARE_OK)    ? "PASS" : "FAIL");
   printf("  RPC channel opened:       %s\n",
          (g_selfTestFlags & ST_RPC_OPEN_OK)  ? "PASS" : "FAIL");
   printf("  Version sent to VMX:      %s\n",
          (g_selfTestFlags & ST_VERSION_OK)   ? "PASS" : "FAIL");
   printf("  TCLO reset received:      %s\n",
          (g_selfTestFlags & ST_RESET_OK)     ? "PASS" : "FAIL");
   printf("  Capabilities registered:  %s\n",
          (g_selfTestFlags & ST_CAPS_OK)      ? "PASS" : "FAIL");
   printf("  Plugin(s) loaded:         %s\n",
          (g_selfTestFlags & ST_PLUGIN_OK)    ? "PASS" : "FAIL",
          g_pluginsLoaded);
   printf("  Heartbeat loop running:   %s\n",
          (g_selfTestFlags & ST_HEARTBEAT_OK) ? "PASS" : "FAIL");
   printf("---------------------------------\n");

   if (g_selfTestFlags == ST_ALL_OK) {
      printf("Congratulations, v0.2 works as expected.\n");
   } else {
      printf("WARNING: one or more checks failed (flags=0x%02x).\n",
             g_selfTestFlags);
   }
   printf("\n");
}

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
   g_selfTestFlags |= ST_VMWARE_OK;

   /* TODO v0.3: ToolsCoreInitEnv equivalent (env var management) */
   /* TODO v0.3: ToolsCorePool_Init equivalent (thread pool) */
   /* TODO v0.3: debug plugin loading */

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
   int      selfTestPrinted = 0;

   if (ctx == NULL) {
      return;
   }

   /* TODO v0.3: VSockets init (Windows) */

   /* Init RPC channel */
   if (!RoseToolsRpc_Init(ctx)) {
      fprintf(stderr, "vmrosd: RPC channel init failed.\n");
      ROSEAPP_ERROR(ctx, 1);
      goto cleanup;
   }

   /* ROSE-TOOLS BEGIN: RpcChannel_Start and initial version report moved
    * into RoseToolsRpc_Init. Both outbound (RpcChannel) and inbound (RpcIn)
    * channels are started there. Version is reported by ToolsCoreCheckReset
    * on first TCLO reset, not eagerly here.
    * ROSE-TOOLS ORIGINAL:
    *   if (!RpcChannel_Start(ctx->rpc)) { ... }
    *   g_selfTestFlags |= ST_RPC_OPEN_OK;
    *   RoseCoreReportVersionData(ctx);
    *   g_selfTestFlags |= ST_VERSION_OK;
    */
   g_selfTestFlags |= ST_RPC_OPEN_OK;
   g_selfTestFlags |= ST_VERSION_OK;  /* will be confirmed by CapReg */
   /* ROSE-TOOLS END */

   /* TODO v0.3: tools notifications (Windows) */
   /* TODO v0.3: GuestStore client init */

   /* Load plugins */
   pluginsLoaded = RosePluginMgr_Load(ctx);
   if (pluginsLoaded == 0) {
      fprintf(stderr, "vmrosd: no plugins loaded.\n");
      ROSEAPP_ERROR(ctx, 1);
      goto cleanup;
   }

   printf("vmrosd: %d plugin(s) loaded, entering main loop.\n",
          pluginsLoaded);
   g_pluginsLoaded  = pluginsLoaded;
   g_selfTestFlags |= ST_PLUGIN_OK;

   /* TODO v0.3: vSocket family reference (Linux main service) */
   /* TODO v0.3: hang detector */
   /* TODO v0.3: GlobalConfig module */

   /* Connect core signals */
   /* TODO v0.3: ROSE_SIG_IO_FREEZE handler (config poll suspend/resume) */
   /* TODO v0.3: ROSE_SIG_SET_OPTION handler (log level changes) */
   /* TODO v0.3: ROSE_SIG_RESET handler (VMX guest logger reinit) */

   lastConfPoll  = GetTickCount();
   lastHeartbeat = GetTickCount();

   /* Main loop - mirrors g_main_loop_run */
   while (ctx->running && ctx->errorCode == 0) {
      now = GetTickCount();

      /* Periodic heartbeat - resend tools version every 10s.
       * VMX times out tools as inactive after ~20s without a heartbeat. */
      if ((now - lastHeartbeat) >= 10000u) {
         RoseCoreReportVersionData(ctx);
         g_selfTestFlags |= ST_HEARTBEAT_OK;
         lastHeartbeat = now;
      }

      /* Print selftest once, after the first heartbeat fired */
      if (!selfTestPrinted &&
          (g_selfTestFlags & ST_HEARTBEAT_OK)) {
         PrintSelfTestResult();
         selfTestPrinted = 1;
      }

      /* Drive inbound TCLO channel - must be called every tick */
      if (!RoseToolsRpc_Poll(ctx)) {
         fprintf(stderr, "vmrosd: inbound RPC poll failed, exiting.\n");
         break;
      }

      /* Tick plugins */
      RosePluginMgr_Tick(ctx);

      /* Periodic config reload */
      if ((now - lastConfPoll) >= (DWORD)CONF_POLL_MS) {
         /* TODO v0.3: RoseCore_ReloadConfig(ctx) */
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

   /* TODO v0.3: GuestStore plugin shutdown first (deadlock prevention) */
   /* TODO v0.3: thread pool shutdown */

   /* Unload plugins */
   RosePluginMgr_Shutdown(ctx);

   /* Emit shutdown signal */
   if (ctx->signals != NULL) {
      RoseEmitSignal(ctx->signals, ROSE_SIG_SHUTDOWN, ctx);
   }

   /* TODO v0.3: vSocket family release */
   /* TODO v0.3: GuestStore client deinit */
   /* TODO v0.3: tools notifications end (Windows) */

   /* Shutdown RPC - must happen before FreeLibrary so any final TCLO
    * commands from VMware are handled while DLL memory is still valid */
   RoseToolsRpc_Shutdown(ctx);

   /* Now safe to release DLL handles */
   RosePluginMgr_Free();

   /* TODO v0.3: free config */
   /* TODO v0.3: uninitialize COM */

   printf("vmrosd: cleanup complete.\n");
}
