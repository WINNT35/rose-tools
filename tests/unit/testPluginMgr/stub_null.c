/*
 * Copyright (C) 2026 WINNT35
 *
 * stub_null.c -- test fixture DLL
 *
 * Exports RosePluginOnLoad but returns NULL.
 * Used by testpluginmgr to verify that a plugin that declines to load
 * is skipped cleanly and not counted.
 */
#include <windows.h>
#include "vmware/tools/rose_plugin.h"

ROSE_MODULE_EXPORT RosePluginData *
RoseOnLoad(RoseAppCtx *ctx)
{
   (void)ctx;
   return NULL;
}
