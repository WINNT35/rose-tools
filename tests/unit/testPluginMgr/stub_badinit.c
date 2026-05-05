/*
 * Copyright (C) 2026 WINNT35
 *
 * stub_badinit.c -- test fixture DLL
 *
 * Exports RosePluginOnLoad with a valid name but whose init() returns 0.
 * Used by testpluginmgr to verify that a plugin whose init fails is
 * skipped and not counted.
 */
#include <windows.h>
#include "vmware/tools/rose_plugin.h"

static int BadInit(RoseAppCtx *ctx) { (void)ctx; return 0; }

static RosePluginData g_data = {
   "stub_badinit",  /* name */
   {{0}},           /* regs */
   0,               /* regCount */
   BadInit,         /* init */
   NULL,            /* tick */
   NULL,            /* shutdown */
   NULL,            /* errorCb */
   0,               /* tickIntervalMs */
   NULL             /* _private */
};

ROSE_MODULE_EXPORT RosePluginData *
RoseOnLoad(RoseAppCtx *ctx)
{
   (void)ctx;
   return &g_data;
}
