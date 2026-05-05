/*
 * Copyright (C) 2026 WINNT35
 *
 * stub_valid.c -- test fixture DLL
 *
 * Exports RosePluginOnLoad, returns a valid RosePluginData.
 * Used by testpluginmgr to verify successful plugin loading.
 */
#include <windows.h>
#include "vmware/tools/rose_plugin.h"

static int stub_init_called    = 0;
static int stub_tick_called    = 0;
static int stub_shutdown_called = 0;

static int StubInit(RoseAppCtx *ctx)     { (void)ctx; stub_init_called = 1; return 1; }
static int StubTick(RoseAppCtx *ctx)     { (void)ctx; stub_tick_called = 1; return 1; }
static void StubShutdown(RoseAppCtx *ctx){ (void)ctx; stub_shutdown_called = 1; }

static RosePluginData g_data = {
   "stub_valid",    /* name */
   {{0}},           /* regs */
   0,               /* regCount */
   StubInit,        /* init */
   StubTick,        /* tick */
   StubShutdown,    /* shutdown */
   NULL,            /* errorCb */
   1000,            /* tickIntervalMs */
   NULL             /* _private */
};

ROSE_MODULE_EXPORT RosePluginData *
RoseOnLoad(RoseAppCtx *ctx)
{
   (void)ctx;
   return &g_data;
}
