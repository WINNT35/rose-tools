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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* ROSE-TOOLS BEGIN: runtime temp dir resolution so the test works on
 * both Linux (/tmp/) and Windows (GetTempPathA) without hardcoding.
 * Populated once in main() before any test runs. */
static char g_tmpdir[512];
/* ROSE-TOOLS END */

/* _snprintf is MinGW/MSVC; Linux gcc uses snprintf */
#ifndef _WIN32
#  define _snprintf snprintf
#endif

#include "roseConfig.h"

/* -----------------------------------------------------------------------
 * Test framework - minimal, no dependencies
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

#define CHECK_STR(desc, actual, expected) \
   CHECK(desc, (actual) != NULL && strcmp((actual), (expected)) == 0)

#define CHECK_NULL(desc, actual) \
   CHECK(desc, (actual) == NULL)

#define CHECK_INT(desc, actual, expected) \
   CHECK(desc, (actual) == (expected))


/* -----------------------------------------------------------------------
 * Helper: write a temp file with given content, return path
 * --------------------------------------------------------------------- */
/* Builds a full path in g_tmpdir. Returns a pointer to a static buffer.
 * Not re-entrant but fine for sequential test use. */
static const char *
MakePath(const char *filename)
{
   static char buf[512];
   _snprintf(buf, sizeof buf, "%s%s", g_tmpdir, filename);
   buf[sizeof buf - 1] = '\0';
   return buf;
}


static void
WriteTempFile(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   if (f == NULL) {
      fprintf(stderr, "test_config: cannot write temp file '%s'\n", path);
      exit(1);
   }
   fputs(content, f);
   fclose(f);
}


static void
RemoveTempFile(const char *path)
{
#ifdef _WIN32
   DeleteFileA(path);
#else
   unlink(path);
#endif
}


/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

static void
TestMissingFile(void)
{
   RoseConfig cfg;
   int ret;

   printf("\n[Test] Missing file\n");
   memset(&cfg, 0, sizeof cfg);
   ret = RoseConfig_Load(&cfg, MakePath("rose_test_nonexistent_xyzzy.conf"));
   CHECK("returns zero for missing file",    ret == 0);
   CHECK("sectionCount is zero",             cfg.sectionCount == 0);
}


static void
TestEmptyFile(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_empty.conf");

   printf("\n[Test] Empty file\n");
   WriteTempFile(path, "");
   memset(&cfg, 0, sizeof cfg);
   CHECK("load returns non-zero",  RoseConfig_Load(&cfg, path));
   CHECK("sectionCount is zero",   cfg.sectionCount == 0);
   RemoveTempFile(path);
}


static void
TestImplicitSection(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_implicit.conf");
   const char *content =
      "# rose-tools config\n"
      "key1 = value1\n"
      "key2=value2\n";

   printf("\n[Test] Implicit [vmtools] section\n");
   WriteTempFile(path, content);
   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   CHECK_STR("key1 in vmtools",          RoseConfig_GetString(&cfg, "vmtools", "key1"), "value1");
   CHECK_STR("key2 in vmtools no space", RoseConfig_GetString(&cfg, "vmtools", "key2"), "value2");
   CHECK_NULL("missing key returns NULL", RoseConfig_GetString(&cfg, "vmtools", "missing"));
   RemoveTempFile(path);
}


static void
TestExplicitSections(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_sections.conf");
   const char *content =
      "[vmtools]\n"
      "log = TRUE\n"
      "\n"
      "[timesync]\n"
      "enabled = true\n"
      "period = 30\n"
      "\n"
      "[guestInfo]\n"
      "poll-interval = 5\n";

   printf("\n[Test] Explicit sections\n");
   WriteTempFile(path, content);
   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   CHECK_STR("vmtools.log",             RoseConfig_GetString(&cfg, "vmtools",  "log"),           "TRUE");
   CHECK_STR("timesync.enabled",        RoseConfig_GetString(&cfg, "timesync", "enabled"),        "true");
   CHECK_STR("timesync.period",         RoseConfig_GetString(&cfg, "timesync", "period"),         "30");
   CHECK_STR("guestInfo.poll-interval", RoseConfig_GetString(&cfg, "guestInfo","poll-interval"),  "5");
   CHECK_NULL("wrong section",          RoseConfig_GetString(&cfg, "timesync", "log"));
   CHECK_INT("sectionCount is 3",       cfg.sectionCount, 3);
   RemoveTempFile(path);
}


static void
TestWhitespaceStripping(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_whitespace.conf");
   const char *content =
      "[vmtools]\n"
      "  key1  =  value with spaces  \n"
      "\tkey2\t=\ttabbed\t\n"
      "key3 =\n";   /* empty value */

   printf("\n[Test] Whitespace stripping\n");
   WriteTempFile(path, content);
   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   CHECK_STR("leading/trailing whitespace stripped", RoseConfig_GetString(&cfg, "vmtools", "key1"), "value with spaces");
   CHECK_STR("tabs stripped",                        RoseConfig_GetString(&cfg, "vmtools", "key2"), "tabbed");
   CHECK_STR("empty value ok",                       RoseConfig_GetString(&cfg, "vmtools", "key3"), "");
   RemoveTempFile(path);
}


static void
TestComments(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_comments.conf");
   const char *content =
      "# full line hash comment\n"
      "; full line semicolon comment\n"
      "  # indented comment\n"
      "[vmtools]\n"
      "real = yes\n"
      "# another comment\n"
      "; yet another\n";

   printf("\n[Test] Comments ignored\n");
   WriteTempFile(path, content);
   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   CHECK_STR("real key parsed",     RoseConfig_GetString(&cfg, "vmtools", "real"), "yes");
   CHECK_INT("sectionCount is 1",   cfg.sectionCount, 1);
   CHECK_INT("keyCount is 1",       cfg.sections[0].keyCount, 1);
   RemoveTempFile(path);
}


static void
TestAccessors(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_accessors.conf");
   const char *content =
      "[plugin]\n"
      "count = 42\n"
      "enabled = true\n"
      "disabled = false\n"
      "flag1 = 1\n"
      "flag0 = 0\n"
      "garbage = notabool\n";

   printf("\n[Test] GetInt and GetBool accessors\n");
   WriteTempFile(path, content);
   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   CHECK_INT("GetInt count=42",           RoseConfig_GetInt(&cfg,  "plugin", "count",   0),  42);
   CHECK_INT("GetInt missing uses defVal", RoseConfig_GetInt(&cfg,  "plugin", "missing", 99), 99);
   CHECK_INT("GetBool true=1",            RoseConfig_GetBool(&cfg, "plugin", "enabled",  0),  1);
   CHECK_INT("GetBool false=0",           RoseConfig_GetBool(&cfg, "plugin", "disabled", 1),  0);
   CHECK_INT("GetBool 1=1",               RoseConfig_GetBool(&cfg, "plugin", "flag1",    0),  1);
   CHECK_INT("GetBool 0=0",               RoseConfig_GetBool(&cfg, "plugin", "flag0",    1),  0);
   CHECK_INT("GetBool garbage=defVal",    RoseConfig_GetBool(&cfg, "plugin", "garbage",  7),  7);
   CHECK_INT("GetBool missing=defVal",    RoseConfig_GetBool(&cfg, "plugin", "missing",  3),  3);
   RemoveTempFile(path);
}


static void
TestDuplicateKey(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_dup.conf");
   const char *content =
      "[vmtools]\n"
      "key = first\n"
      "key = second\n";

   printf("\n[Test] Duplicate key - last value wins\n");
   WriteTempFile(path, content);
   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   CHECK_STR("last value wins",    RoseConfig_GetString(&cfg, "vmtools", "key"), "second");
   CHECK_INT("keyCount is still 1", cfg.sections[0].keyCount, 1);
   RemoveTempFile(path);
}


static void
TestLimits(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_limits.conf");
   FILE       *f;
   int         i;

   printf("\n[Test] Section and key limits not exceeded\n");

   /* Write more sections than ROSE_MAX_CONFIG_SECTIONS */
   f = fopen(path, "w");
   if (f == NULL) { printf("  [SKIP] cannot write temp file\n"); return; }
   for (i = 0; i < ROSE_MAX_CONFIG_SECTIONS + 10; i++) {
      fprintf(f, "[section%d]\nkey = val\n", i);
   }
   fclose(f);

   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);
   CHECK("section count capped at limit", cfg.sectionCount <= ROSE_MAX_CONFIG_SECTIONS);
   RemoveTempFile(path);

   /* Write more keys than ROSE_MAX_CONFIG_KEYS in one section */
   f = fopen(path, "w");
   if (f == NULL) { printf("  [SKIP] cannot write temp file\n"); return; }
   fprintf(f, "[vmtools]\n");
   for (i = 0; i < ROSE_MAX_CONFIG_KEYS + 10; i++) {
      fprintf(f, "key%d = val%d\n", i, i);
   }
   fclose(f);

   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);
   CHECK("key count capped at limit", cfg.sections[0].keyCount <= ROSE_MAX_CONFIG_KEYS);
   RemoveTempFile(path);
}


static void
TestLongLines(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_longline.conf");
   FILE       *f;
   char        longval[600];
   int         i;

   printf("\n[Test] Long values truncated safely\n");

   /* Build a value longer than ROSE_CONFIG_STR_MAX */
   for (i = 0; i < (int)sizeof longval - 1; i++) {
      longval[i] = 'a' + (i % 26);
   }
   longval[sizeof longval - 1] = '\0';

   f = fopen(path, "w");
   if (f == NULL) { printf("  [SKIP] cannot write temp file\n"); return; }
   fprintf(f, "[vmtools]\nlong = %s\n", longval);
   fclose(f);

   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   {
      const char *v = RoseConfig_GetString(&cfg, "vmtools", "long");
      CHECK("long value not NULL",
            v != NULL);
      CHECK("long value truncated to limit",
            v != NULL && strlen(v) < ROSE_CONFIG_STR_MAX);
   }
   RemoveTempFile(path);
}


static void
TestClear(void)
{
   RoseConfig  cfg;
   const char *path = MakePath("rose_test_clear.conf");
   const char *content = "[vmtools]\nkey = value\n";

   printf("\n[Test] RoseConfig_Clear\n");
   WriteTempFile(path, content);
   memset(&cfg, 0, sizeof cfg);
   RoseConfig_Load(&cfg, path);

   CHECK("has data before clear", cfg.sectionCount > 0);

   RoseConfig_Clear(&cfg);
   CHECK("sectionCount zero after clear",  cfg.sectionCount == 0);
   CHECK_NULL("GetString NULL after clear", RoseConfig_GetString(&cfg, "vmtools", "key"));
   RemoveTempFile(path);
}


/* -----------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
   (void)argc;
   (void)argv;

   /* Resolve temp directory at runtime */
#ifdef _WIN32
   {
      DWORD n = GetTempPathA((DWORD)sizeof g_tmpdir, g_tmpdir);
      if (n == 0 || n >= (DWORD)sizeof g_tmpdir) {
         fprintf(stderr, "test_config: GetTempPathA failed\n");
         return 1;
      }
      /* GetTempPathA already appends a backslash */
   }
#else
   _snprintf(g_tmpdir, sizeof g_tmpdir, "/tmp/");
#endif

   printf("test_config: rose-tools config parser unit test\n");
   printf("test_config: (runs on build host, no VMware required)\n");
   printf("test_config: using temp dir: %s\n", g_tmpdir);

   TestMissingFile();
   TestEmptyFile();
   TestImplicitSection();
   TestExplicitSections();
   TestWhitespaceStripping();
   TestComments();
   TestAccessors();
   TestDuplicateKey();
   TestLimits();
   TestLongLines();
   TestClear();

   printf("\n--- test_config results ---\n");
   printf("  %d / %d passed\n\n", g_passed, g_passed + g_failed);

   return g_failed > 0 ? 1 : 0;
}

