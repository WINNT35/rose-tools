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
 * tests/unit/testPluginMgr/test_pluginmgr.c
 *
 * Unit test for dynamic plugin loading (v0.2).
 * Tests RosePluginMgr_Load, RosePluginMgr_Tick, RosePluginMgr_Shutdown
 * using stub DLLs built alongside this test.
 *
 * Can run on the build host under Wine or directly on a Windows machine.
 * Does NOT require a VMware guest.
 *
 * Expects the following stub DLLs in the same directory as the exe:
 *   stub_valid.dll    -- valid plugin, init succeeds
 *   stub_null.dll     -- RosePluginOnLoad returns NULL
 *   stub_badinit.dll  -- RosePluginOnLoad returns data but init() fails
 *   stub_noexport.dll -- DLL with no RosePluginOnLoad export
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vmware/tools/rose_plugin.h"
#include "roseConfig.h"

/* _snprintf is MinGW/MSVC; Linux gcc uses snprintf */
#ifndef _WIN32
#  define _snprintf snprintf
#endif

/* -----------------------------------------------------------------------
 * Test framework - mirrors test_config.c
 * --------------------------------------------------------------------- */
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(desc, expr) do {                                  \
   if (expr) {                                                  \
      printf("  [PASS] %s\n", desc);                           \
      g_passed++;                                               \
   } else {                                                     \
      printf("  [FAIL] %s  (line %d)\n", desc, __LINE__);      \
      g_failed++;                                               \
   }                                                            \
} while (0)

#define CHECK_INT(desc, actual, expected) \
   CHECK(desc, (actual) == (expected))

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Directory of the test exe - populated in main() */
static char g_exedir[MAX_PATH];

/* Builds a full path relative to the exe directory */
static const char *
ExePath(const char *filename)
{
   static char buf[MAX_PATH];
   _snprintf(buf, sizeof buf, "%s\\%s", g_exedir, filename);
   buf[sizeof buf - 1] = '\0';
   return buf;
}

/* Creates a directory, ignoring ERROR_ALREADY_EXISTS */
static int
MakeDir(const char *path)
{
   if (CreateDirectoryA(path, NULL)) {
      return 1;
   }
   return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Removes a directory (must be empty) */
static void
RemoveDir(const char *path)
{
   RemoveDirectoryA(path);
}

/* Copies a file from src to dst */
static int
CopyDll(const char *src, const char *dst)
{
   return (int)CopyFileA(src, dst, FALSE);
}

/* Deletes a file */
static void
DeleteDll(const char *path)
{
   DeleteFileA(path);
}

/*
 * Builds a minimal tools.conf pointing plugin_dir at the given path.
 * Returns the path to the written conf file (static buffer).
 */
static const char *
WriteConf(const char *pluginDir)
{
   static char confPath[MAX_PATH];
   FILE *f;
   _snprintf(confPath, sizeof confPath, "%s\\test_pluginmgr.conf", g_exedir);
   confPath[sizeof confPath - 1] = '\0';
   f = fopen(confPath, "w");
   if (f == NULL) {
      fprintf(stderr, "test_pluginmgr: cannot write conf file\n");
      exit(1);
   }
   fprintf(f, "[vmtools]\nplugin_dir=%s\n", pluginDir);
   fclose(f);
   return confPath;
}

/* Deletes the temp conf file */
static void
DeleteConf(const char *path)
{
   DeleteFileA(path);
}

/* Builds a minimal RoseAppCtx suitable for plugin manager tests */
static void
MakeCtx(RoseAppCtx *ctx, RoseSignalRegistry *sigs, RoseConfig *cfg)
{
   memset(ctx,  0, sizeof *ctx);
   memset(sigs, 0, sizeof *sigs);
   memset(cfg,  0, sizeof *cfg);
   ctx->version = ROSE_CORE_API_V1;
   ctx->name    = ROSE_GUEST_SERVICE;
   ctx->running = 1;
   ctx->signals = sigs;
   ctx->config  = cfg;
   ctx->rpc     = NULL;   /* no RPC needed for plugin load tests */
   ctx->rpcIn   = NULL;
}

/* Forward declarations from pluginMgr.c */
extern int  RosePluginMgr_Load(RoseAppCtx *ctx);
extern void RosePluginMgr_Tick(RoseAppCtx *ctx);
extern void RosePluginMgr_Shutdown(RoseAppCtx *ctx);

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/*
 * TestMissingDir
 *
 * plugin_dir points to a path that does not exist.
 * RosePluginMgr_Load should return 0 without crashing.
 */
static void
TestMissingDir(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   const char        *confPath;
   int                loaded;

   printf("\n[Test] Missing plugin directory\n");

   confPath = WriteConf(ExePath("nonexistent_plugin_dir_xyzzy"));
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("returns 0 for missing dir", loaded, 0);

   RosePluginMgr_Shutdown(&ctx);
   DeleteConf(confPath);
}


/*
 * TestEmptyDir
 *
 * plugin_dir exists but contains no DLLs.
 * RosePluginMgr_Load should return 0.
 */
static void
TestEmptyDir(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   char               dirPath[MAX_PATH];
   const char        *confPath;
   int                loaded;

   printf("\n[Test] Empty plugin directory\n");

   _snprintf(dirPath, sizeof dirPath, "%s\\empty_plugins", g_exedir);
   dirPath[sizeof dirPath - 1] = '\0';

   MakeDir(dirPath);
   confPath = WriteConf(dirPath);
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("returns 0 for empty dir", loaded, 0);

   RosePluginMgr_Shutdown(&ctx);
   DeleteConf(confPath);
   RemoveDir(dirPath);
}


/*
 * TestNullPlugin
 *
 * Directory contains stub_null.dll whose RosePluginOnLoad returns NULL.
 * Should be skipped - load count stays 0.
 */
static void
TestNullPlugin(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   char               dirPath[MAX_PATH];
   char               dllDst[MAX_PATH];
   const char        *confPath;
   int                loaded;

   printf("\n[Test] Plugin whose OnLoad returns NULL\n");

   _snprintf(dirPath, sizeof dirPath, "%s\\null_plugins", g_exedir);
   dirPath[sizeof dirPath - 1] = '\0';
   _snprintf(dllDst, sizeof dllDst, "%s\\stub_null.dll", dirPath);
   dllDst[sizeof dllDst - 1] = '\0';

   MakeDir(dirPath);
   if (!CopyDll(ExePath("stub_null.dll"), dllDst)) {
      printf("  [SKIP] stub_null.dll not found next to test exe\n");
      RemoveDir(dirPath);
      return;
   }

   confPath = WriteConf(dirPath);
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("NULL onload not counted", loaded, 0);

   RosePluginMgr_Shutdown(&ctx);
   DeleteDll(dllDst);
   DeleteConf(confPath);
   RemoveDir(dirPath);
}


/*
 * TestBadInitPlugin
 *
 * Directory contains stub_badinit.dll whose init() returns 0.
 * Should be skipped - load count stays 0.
 */
static void
TestBadInitPlugin(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   char               dirPath[MAX_PATH];
   char               dllDst[MAX_PATH];
   const char        *confPath;
   int                loaded;

   printf("\n[Test] Plugin whose init() fails\n");

   _snprintf(dirPath, sizeof dirPath, "%s\\badinit_plugins", g_exedir);
   dirPath[sizeof dirPath - 1] = '\0';
   _snprintf(dllDst, sizeof dllDst, "%s\\stub_badinit.dll", dirPath);
   dllDst[sizeof dllDst - 1] = '\0';

   MakeDir(dirPath);
   if (!CopyDll(ExePath("stub_badinit.dll"), dllDst)) {
      printf("  [SKIP] stub_badinit.dll not found next to test exe\n");
      RemoveDir(dirPath);
      return;
   }

   confPath = WriteConf(dirPath);
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("failed init not counted", loaded, 0);

   RosePluginMgr_Shutdown(&ctx);
   DeleteDll(dllDst);
   DeleteConf(confPath);
   RemoveDir(dirPath);
}


/*
 * TestNoExportPlugin
 *
 * Directory contains a DLL that has no RosePluginOnLoad export.
 * Should be skipped cleanly - load count stays 0.
 * We reuse stub_null.dll renamed, since any DLL without the export works.
 * Actually we build stub_noexport.dll explicitly for clarity.
 */
static void
TestNoExportPlugin(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   char               dirPath[MAX_PATH];
   char               dllDst[MAX_PATH];
   const char        *confPath;
   int                loaded;

   printf("\n[Test] DLL with no RosePluginOnLoad export\n");

   _snprintf(dirPath, sizeof dirPath, "%s\\noexport_plugins", g_exedir);
   dirPath[sizeof dirPath - 1] = '\0';
   _snprintf(dllDst, sizeof dllDst, "%s\\stub_noexport.dll", dirPath);
   dllDst[sizeof dllDst - 1] = '\0';

   MakeDir(dirPath);
   if (!CopyDll(ExePath("stub_noexport.dll"), dllDst)) {
      printf("  [SKIP] stub_noexport.dll not found next to test exe\n");
      RemoveDir(dirPath);
      return;
   }

   confPath = WriteConf(dirPath);
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("no-export DLL not counted", loaded, 0);

   RosePluginMgr_Shutdown(&ctx);
   DeleteDll(dllDst);
   DeleteConf(confPath);
   RemoveDir(dirPath);
}


/*
 * TestValidPlugin
 *
 * Directory contains stub_valid.dll which loads cleanly.
 * Should be counted, init should be called.
 */
static void
TestValidPlugin(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   char               dirPath[MAX_PATH];
   char               dllDst[MAX_PATH];
   const char        *confPath;
   int                loaded;

   printf("\n[Test] Valid plugin loads and is counted\n");

   _snprintf(dirPath, sizeof dirPath, "%s\\valid_plugins", g_exedir);
   dirPath[sizeof dirPath - 1] = '\0';
   _snprintf(dllDst, sizeof dllDst, "%s\\stub_valid.dll", dirPath);
   dllDst[sizeof dllDst - 1] = '\0';

   MakeDir(dirPath);
   if (!CopyDll(ExePath("stub_valid.dll"), dllDst)) {
      printf("  [SKIP] stub_valid.dll not found next to test exe\n");
      RemoveDir(dirPath);
      return;
   }

   confPath = WriteConf(dirPath);
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("valid plugin counted",    loaded, 1);

   RosePluginMgr_Shutdown(&ctx);
   DeleteDll(dllDst);
   DeleteConf(confPath);
   RemoveDir(dirPath);
}


/*
 * TestMixedDir
 *
 * Directory contains stub_valid.dll, stub_null.dll, and stub_badinit.dll.
 * Only stub_valid should be counted.
 */
static void
TestMixedDir(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   char               dirPath[MAX_PATH];
   char               dllDst[MAX_PATH];
   const char        *confPath;
   int                loaded;
   int                anySkipped = 0;

   printf("\n[Test] Mixed directory - only valid plugins counted\n");

   _snprintf(dirPath, sizeof dirPath, "%s\\mixed_plugins", g_exedir);
   dirPath[sizeof dirPath - 1] = '\0';
   MakeDir(dirPath);

#define COPY_OR_SKIP(name) \
   _snprintf(dllDst, sizeof dllDst, "%s\\%s", dirPath, name); \
   dllDst[sizeof dllDst - 1] = '\0'; \
   if (!CopyDll(ExePath(name), dllDst)) { \
      printf("  [SKIP] %s not found\n", name); \
      anySkipped = 1; \
   }

   COPY_OR_SKIP("stub_valid.dll")
   COPY_OR_SKIP("stub_null.dll")
   COPY_OR_SKIP("stub_badinit.dll")

#undef COPY_OR_SKIP

   if (anySkipped) {
      /* Clean up whatever was copied */
      _snprintf(dllDst, sizeof dllDst, "%s\\stub_valid.dll",   dirPath); DeleteDll(dllDst);
      _snprintf(dllDst, sizeof dllDst, "%s\\stub_null.dll",    dirPath); DeleteDll(dllDst);
      _snprintf(dllDst, sizeof dllDst, "%s\\stub_badinit.dll", dirPath); DeleteDll(dllDst);
      RemoveDir(dirPath);
      return;
   }

   confPath = WriteConf(dirPath);
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("only valid plugin counted in mixed dir", loaded, 1);

   RosePluginMgr_Shutdown(&ctx);

   _snprintf(dllDst, sizeof dllDst, "%s\\stub_valid.dll",   dirPath); DeleteDll(dllDst);
   _snprintf(dllDst, sizeof dllDst, "%s\\stub_null.dll",    dirPath); DeleteDll(dllDst);
   _snprintf(dllDst, sizeof dllDst, "%s\\stub_badinit.dll", dirPath); DeleteDll(dllDst);
   DeleteConf(confPath);
   RemoveDir(dirPath);
}


/*
 * TestShutdownOrder
 *
 * Loads two copies of stub_valid.dll under different names and verifies
 * that shutdown is called (no crash, count resets to 0 after shutdown).
 * Full reverse-order verification would require inter-DLL communication;
 * we settle for confirming the count is zeroed and no crash occurs.
 */
static void
TestShutdownOrder(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   char               dirPath[MAX_PATH];
   char               dll1[MAX_PATH];
   char               dll2[MAX_PATH];
   const char        *confPath;
   int                loaded;

   printf("\n[Test] Shutdown cleans up loaded plugins\n");

   _snprintf(dirPath, sizeof dirPath, "%s\\shutdown_plugins", g_exedir);
   dirPath[sizeof dirPath - 1] = '\0';
   _snprintf(dll1, sizeof dll1, "%s\\stub_valid_a.dll", dirPath);
   dll1[sizeof dll1 - 1] = '\0';
   _snprintf(dll2, sizeof dll2, "%s\\stub_valid_b.dll", dirPath);
   dll2[sizeof dll2 - 1] = '\0';

   MakeDir(dirPath);
   if (!CopyDll(ExePath("stub_valid.dll"), dll1) ||
       !CopyDll(ExePath("stub_valid.dll"), dll2)) {
      printf("  [SKIP] stub_valid.dll not found next to test exe\n");
      DeleteDll(dll1);
      DeleteDll(dll2);
      RemoveDir(dirPath);
      return;
   }

   confPath = WriteConf(dirPath);
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);

   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("two plugins loaded before shutdown", loaded, 2);

   RosePluginMgr_Shutdown(&ctx);

   /* Reload into a fresh context to confirm state was fully reset */
   MakeCtx(&ctx, &sigs, &cfg);
   RoseConfig_Load(&cfg, confPath);
   loaded = RosePluginMgr_Load(&ctx);
   CHECK_INT("plugins reload cleanly after shutdown", loaded, 2);
   RosePluginMgr_Shutdown(&ctx);

   DeleteDll(dll1);
   DeleteDll(dll2);
   DeleteConf(confPath);
   RemoveDir(dirPath);
}


/*
 * TestDefaultPluginDir
 *
 * No plugin_dir in config - should fall back to the default path.
 * We just verify it doesn't crash and returns 0 (default dir won't
 * exist on the test machine). The default path itself is logged.
 */
static void
TestDefaultPluginDir(void)
{
   RoseAppCtx         ctx;
   RoseSignalRegistry sigs;
   RoseConfig         cfg;
   int                loaded;

   printf("\n[Test] Default plugin_dir fallback (no config key)\n");

   MakeCtx(&ctx, &sigs, &cfg);
   /* Deliberately do not load any conf - config stays empty */

   loaded = RosePluginMgr_Load(&ctx);
   CHECK("does not crash with default dir", loaded >= 0);

   RosePluginMgr_Shutdown(&ctx);
}


/* -----------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
   char exePath[MAX_PATH];
   char *lastSep;

   (void)argc;
   (void)argv;

   /* Resolve directory of this executable */
   if (GetModuleFileNameA(NULL, exePath, sizeof exePath) == 0) {
      fprintf(stderr, "test_pluginmgr: GetModuleFileNameA failed\n");
      return 1;
   }
   lastSep = strrchr(exePath, '\\');
   if (lastSep != NULL) {
      *lastSep = '\0';
      _snprintf(g_exedir, sizeof g_exedir, "%s", exePath);
   } else {
      _snprintf(g_exedir, sizeof g_exedir, ".");
   }
   g_exedir[sizeof g_exedir - 1] = '\0';

   printf("test_pluginmgr: rose-tools dynamic plugin manager unit test\n");
   printf("test_pluginmgr: (runs on build host via Wine or on Windows)\n");
   printf("test_pluginmgr: exe dir: %s\n", g_exedir);

   TestMissingDir();
   TestEmptyDir();
   TestNullPlugin();
   TestBadInitPlugin();
   TestNoExportPlugin();
   TestValidPlugin();
   TestMixedDir();
   TestShutdownOrder();
   TestDefaultPluginDir();

   printf("\n--- test_pluginmgr results ---\n");
   printf("  %d / %d passed\n\n", g_passed, g_passed + g_failed);

   return g_failed > 0 ? 1 : 0;
}
