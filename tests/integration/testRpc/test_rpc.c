/*
 * Copyright (C) 2026 WINNT35
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * rose-tools
 * tests/testRpc/test_rpc.c
 *
 * Integration test for the RPC channel stack.
 * Mirrors the role of open-vm-tools vmrpcdbg test infrastructure,
 * adapted for Win32/C89/no-GLib/no-CUnit.
 *
 * What this tests:
 *   1. TCLO channel opens (RpcIn_start succeeds)
 *   2. VMware sends "reset" and we reply "ATR vmtoolsd"  [RESET_FIRED]
 *   3. vmx.capability.unified_loop is acknowledged       [UNIFIED_LOOP_OK]
 *   4. VMware sends "Capabilities_Register"              [CAPREG_FIRED]
 *   5. tools.set.version* succeeds                       [VERSION_SENT]
 *   6. VMware sends "Set_Option"                         [SETOPT_FIRED]
 *
 * Usage:
 *   testrpc.exe [timeout_seconds]   (default: 15)
 *
 * Exit codes:
 *   0  all assertions passed
 *   1  not running in VMware
 *   2  RPC init failed
 *   3  timeout - one or more assertions did not fire in time
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
#include "rpcin.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"

/* -----------------------------------------------------------------------
 * Assertion flags - set as each event fires
 * --------------------------------------------------------------------- */
#define ASSERT_RESET_FIRED      (1 << 0)
#define ASSERT_UNIFIED_LOOP_OK  (1 << 1)
#define ASSERT_CAPREG_FIRED     (1 << 2)
#define ASSERT_VERSION_SENT     (1 << 3)
#define ASSERT_SETOPT_FIRED     (1 << 4)
#define ASSERT_ALL              (ASSERT_RESET_FIRED    | \
                                 ASSERT_UNIFIED_LOOP_OK | \
                                 ASSERT_CAPREG_FIRED   | \
                                 ASSERT_VERSION_SENT   | \
                                 ASSERT_SETOPT_FIRED)

static unsigned int g_flags        = 0;
static int          g_pendingReset = 0;  /* set by reset handler, processed next tick */

/* -----------------------------------------------------------------------
 * Global test context - mirrors vmrosd.c InitContext
 * --------------------------------------------------------------------- */
static RoseAppCtx         g_ctx;
static RoseSignalRegistry g_signals;
static RoseConfig         g_config;

/* -----------------------------------------------------------------------
 * RpcIn error handler
 * --------------------------------------------------------------------- */
static void
TestRpcError(void *data, char const *errmsg)
{
   (void)data;
   fprintf(stderr, "test_rpc: RPC error: %s\n",
           errmsg ? errmsg : "(null)");
   g_ctx.running = 0;
}

/* -----------------------------------------------------------------------
 * "reset" handler
 *
 * Fires when VMware resets the TCLO channel.
 * Must reply "ATR vmtoolsd". Then sends vmx.capability.unified_loop
 * and checks the reply to set UNIFIED_LOOP_OK.
 * --------------------------------------------------------------------- */
static unsigned int
TestRpcReset(char const **result,
             size_t      *resultLen,
             const char  *name,
             const char  *args,
             size_t       argsSize,
             void        *clientData)
{
   static char reply[64];
   char   buf[256];       /* used for ATR reply */
   char  *ulReply  = NULL; /* used post-reset in poll loop */
   size_t ulRepLen = 0;
   int    ok       = 0;

   (void)name; (void)args; (void)argsSize; (void)clientData;

   g_flags |= ASSERT_RESET_FIRED;
   printf("test_rpc: [PASS] reset fired\n");

   /* Defer unified_loop send to next poll tick - ATR must be delivered
    * to VMware first before we send any outbound RPCs. */
   g_pendingReset = 1;

   (void)buf; (void)ulReply; (void)ulRepLen; (void)ok;

   _snprintf(reply, sizeof reply, "ATR %s", ROSE_GUEST_SERVICE);
   reply[sizeof reply - 1] = '\0';
   return RpcIn_SetRetVals(result, resultLen, reply, TRUE);
}

/* -----------------------------------------------------------------------
 * "Capabilities_Register" handler
 * --------------------------------------------------------------------- */
static unsigned int
TestRpcCapReg(char const **result,
              size_t      *resultLen,
              const char  *name,
              const char  *args,
              size_t       argsSize,
              void        *clientData)
{
   char   buf[256];
   char  *reply  = NULL;
   size_t repLen = 0;
   int    ok;

   (void)name; (void)args; (void)argsSize; (void)clientData;

   g_flags |= ASSERT_CAPREG_FIRED;
   printf("test_rpc: [PASS] Capabilities_Register fired\n");

   /* Try tools.set.versiontype first, fall back to tools.set.version */
   _snprintf(buf, sizeof buf,
             "tools.set.versiontype %u %u",
             (unsigned)TOOLS_VERSION_CURRENT,
             (unsigned)TOOLS_TYPE_MSI);
   buf[sizeof buf - 1] = '\0';

   ok = (int)RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
   if (!ok) {
      free(reply); reply = NULL;
      _snprintf(buf, sizeof buf,
                "tools.set.version %u",
                (unsigned)TOOLS_VERSION_CURRENT);
      buf[sizeof buf - 1] = '\0';
      ok = (int)RpcChannel_SendOneRaw(buf, strlen(buf), &reply, &repLen);
   }

   printf("test_rpc: tools.set.version*: ok=%d repLen=%u reply='%.*s'\n",
          ok, (unsigned)repLen,
          (int)repLen, reply ? reply : "(null)");
   free(reply);

   if (ok) {
      g_flags |= ASSERT_VERSION_SENT;
      printf("test_rpc: [PASS] version sent\n");
   } else {
      printf("test_rpc: [FAIL] version send failed\n");
   }

   return RpcIn_SetRetVals(result, resultLen, "", TRUE);
}

/* -----------------------------------------------------------------------
 * "Set_Option" handler
 * --------------------------------------------------------------------- */
static unsigned int
TestRpcSetOption(char const **result,
                 size_t      *resultLen,
                 const char  *name,
                 const char  *args,
                 size_t       argsSize,
                 void        *clientData)
{
   (void)name; (void)clientData;

   g_flags |= ASSERT_SETOPT_FIRED;
   printf("test_rpc: [PASS] Set_Option fired: '%.*s'\n",
          (int)argsSize, args ? args : "(null)");

   return RpcIn_SetRetVals(result, resultLen, "", TRUE);
}

/* -----------------------------------------------------------------------
 * Print final results
 * --------------------------------------------------------------------- */
static void
PrintResults(void)
{
   static const struct {
      unsigned int flag;
      const char  *name;
   } checks[] = {
      { ASSERT_RESET_FIRED,     "reset fired          " },
      { ASSERT_UNIFIED_LOOP_OK, "unified_loop ok      " },
      { ASSERT_CAPREG_FIRED,    "Capabilities_Register" },
      { ASSERT_VERSION_SENT,    "tools.set.version*   " },
      { ASSERT_SETOPT_FIRED,    "Set_Option fired     " },
   };
   int i;
   int passed = 0;
   int total  = (int)(sizeof checks / sizeof checks[0]);

   printf("\n--- test_rpc results ---\n");
   for (i = 0; i < total; i++) {
      int ok = (g_flags & checks[i].flag) != 0;
      printf("  %s : %s\n", checks[i].name, ok ? "PASS" : "FAIL");
      if (ok) passed++;
   }
   printf("------------------------\n");
   printf("  %d / %d passed\n\n", passed, total);
}

/* -----------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
   RpcIn  *rpcIn = NULL;
   DWORD   startTick;
   DWORD   timeoutMs;
   int     ret = 0;

   /* Parse optional timeout argument */
   timeoutMs = (argc > 1) ? (DWORD)(atoi(argv[1]) * 1000) : 15000;

   printf("test_rpc: rose-tools RPC channel integration test\n");
   printf("test_rpc: timeout=%ums\n\n", (unsigned)timeoutMs);

   /* Must be running in VMware */
   if (!VmCheck_IsVirtualWorld()) {
      fprintf(stderr, "test_rpc: not running in a VMware VM\n");
      return 1;
   }
   printf("test_rpc: running in VMware VM\n");

   /* Init context (no plugins needed for this test) */
   memset(&g_ctx,     0, sizeof g_ctx);
   memset(&g_signals, 0, sizeof g_signals);
   memset(&g_config,  0, sizeof g_config);
   g_ctx.version = ROSE_CORE_API_V1;
   g_ctx.name    = ROSE_GUEST_SERVICE;
   g_ctx.running = 1;
   g_ctx.config  = &g_config;
   g_ctx.signals = &g_signals;
   g_ctx.isVMware = 1;

   /* Create and start outbound channel */
   g_ctx.rpc = RpcChannel_New();
   if (g_ctx.rpc == NULL || !RpcChannel_Start(g_ctx.rpc)) {
      fprintf(stderr, "test_rpc: failed to start outbound channel\n");
      ret = 2;
      goto cleanup;
   }
   printf("test_rpc: outbound channel started\n");

   /* Create inbound channel */
   rpcIn = RpcIn_Construct(&gRoseTimerQueueStorage);
   if (rpcIn == NULL) {
      fprintf(stderr, "test_rpc: failed to construct inbound channel\n");
      ret = 2;
      goto cleanup;
   }

   /* Register handlers */
   RpcIn_RegisterCallback(rpcIn, "Capabilities_Register",
                          TestRpcCapReg, NULL);
   RpcIn_RegisterCallback(rpcIn, "Set_Option",
                          TestRpcSetOption, NULL);

   /* Start - registers "reset" and "ping" automatically */
   if (!RpcIn_start(rpcIn,
                    RPCIN_MAX_DELAY_CS,
                    TestRpcReset,
                    NULL,
                    TestRpcError,
                    NULL,
                    NULL)) {
      fprintf(stderr, "test_rpc: failed to start inbound channel\n");
      ret = 2;
      goto cleanup;
   }
   printf("test_rpc: inbound channel started, polling...\n\n");

   /* Poll loop - run until all assertions pass or timeout */
   startTick = GetTickCount();
   while (g_ctx.running) {
      if (!RpcIn_Poll(rpcIn)) {
         fprintf(stderr, "test_rpc: RpcIn_Poll failed\n");
         break;
      }

      /* Handle deferred post-reset work AFTER RpcIn_Poll so that
       * "OK ATR vmtoolsd" has been delivered before unified_loop goes out. */
      if (g_pendingReset) {
         char   buf[256];
         char  *ulReply  = NULL;
         size_t ulRepLen = 0;
         int    ok;

         g_pendingReset = 0;

         _snprintf(buf, sizeof buf,
                   "vmx.capability.unified_loop %s", ROSE_GUEST_SERVICE);
         buf[sizeof buf - 1] = '\0';
         ok = (int)RpcChannel_SendOneRaw(buf, strlen(buf),
                                         &ulReply, &ulRepLen);
         printf("test_rpc: unified_loop: ok=%d repLen=%u reply='%.*s'\n",
                ok, (unsigned)ulRepLen,
                (int)ulRepLen, ulReply ? ulReply : "(null)");
         if (ok) {
            g_flags |= ASSERT_UNIFIED_LOOP_OK;
            printf("test_rpc: [PASS] unified_loop acknowledged\n");
         } else {
            printf("test_rpc: [FAIL] unified_loop not acknowledged\n");
         }
         free(ulReply);
      }

      if ((g_flags & ASSERT_ALL) == ASSERT_ALL) {
         printf("test_rpc: all assertions passed\n");
         break;
      }

      if ((GetTickCount() - startTick) >= timeoutMs) {
         fprintf(stderr, "test_rpc: TIMEOUT after %ums\n",
                 (unsigned)timeoutMs);
         ret = 3;
         break;
      }

      Sleep(100);
   }

cleanup:
   PrintResults();

   if (rpcIn != NULL) {
      RpcIn_stop(rpcIn);
      RpcIn_Destruct(rpcIn);
   }
   if (g_ctx.rpc != NULL) {
      RpcChannel_Stop(g_ctx.rpc);
      RpcChannel_Destroy(g_ctx.rpc);
   }

   /* Return 0 only if all assertions passed and no error */
   if (ret == 0 && (g_flags & ASSERT_ALL) != ASSERT_ALL) {
      ret = 3;
   }
   return ret;
}

