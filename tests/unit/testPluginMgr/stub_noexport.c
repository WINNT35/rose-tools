/*
 * Copyright (C) 2026 WINNT35
 *
 * stub_noexport.c -- test fixture DLL
 *
 * A valid DLL that does NOT export RosePluginOnLoad.
 * Used by testpluginmgr to verify that DLLs missing the entry point
 * are skipped cleanly without crashing.
 */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
   (void)hinstDLL;
   (void)fdwReason;
   (void)lpvReserved;
   return TRUE;
}
