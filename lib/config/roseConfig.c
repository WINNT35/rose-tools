/*
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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "roseConfig.h"


/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

/*
 * TrimLeft --
 * Returns a pointer into s advanced past any leading whitespace.
 */
static const char *
TrimLeft(const char *s)
{
   while (*s != '\0' && isspace((unsigned char)*s)) {
      s++;
   }
   return s;
}


/*
 * TrimRight --
 * Trims trailing whitespace from s in-place by writing '\0'.
 */
static void
TrimRight(char *s)
{
   int len = (int)strlen(s);
   while (len > 0 && isspace((unsigned char)s[len - 1])) {
      s[--len] = '\0';
   }
}


/*
 * IsComment --
 * Returns non-zero if the trimmed line is a comment or blank.
 */
static int
IsComment(const char *s)
{
   s = TrimLeft(s);
   return (*s == '\0' || *s == '#' || *s == ';');
}


/*
 * ParseSection --
 * If line is a [section] header, copies the section name into out
 * (truncated to ROSE_CONFIG_STR_MAX-1) and returns non-zero.
 * Returns zero if line is not a section header.
 */
static int
ParseSection(const char *line, char *out)
{
   const char *start;
   const char *end;
   size_t      len;

   line = TrimLeft(line);
   if (*line != '[') {
      return 0;
   }
   start = line + 1;
   end   = strchr(start, ']');
   if (end == NULL) {
      return 0;   /* malformed - no closing bracket */
   }

   len = (size_t)(end - start);
   if (len == 0) {
      return 0;   /* empty section name */
   }
   if (len >= ROSE_CONFIG_STR_MAX) {
      len = ROSE_CONFIG_STR_MAX - 1;
   }

   strncpy(out, start, len);
   out[len] = '\0';
   TrimRight(out);
   return 1;
}


/*
 * ParseKeyValue --
 * If line is a key=value pair, copies key and value into the
 * respective output buffers and returns non-zero.
 * Returns zero if no '=' found.
 * Whitespace around '=' is stripped. Value may be empty.
 */
static int
ParseKeyValue(const char *line, char *keyOut, char *valOut)
{
   const char *eq;
   const char *valStart;
   size_t      keyLen;
   size_t      valLen;

   line = TrimLeft(line);
   eq   = strchr(line, '=');
   if (eq == NULL) {
      return 0;
   }

   /* Key: everything before '=', right-trimmed */
   keyLen = (size_t)(eq - line);
   if (keyLen == 0) {
      return 0;   /* empty key */
   }
   if (keyLen >= ROSE_CONFIG_STR_MAX) {
      keyLen = ROSE_CONFIG_STR_MAX - 1;
   }
   strncpy(keyOut, line, keyLen);
   keyOut[keyLen] = '\0';
   TrimRight(keyOut);

   if (keyOut[0] == '\0') {
      return 0;
   }

   /* Value: everything after '=', left-trimmed, right-trimmed */
   valStart = TrimLeft(eq + 1);
   valLen   = strlen(valStart);
   if (valLen >= ROSE_CONFIG_STR_MAX) {
      valLen = ROSE_CONFIG_STR_MAX - 1;
   }
   strncpy(valOut, valStart, valLen);
   valOut[valLen] = '\0';
   TrimRight(valOut);

   return 1;
}


/*
 * FindOrAddSection --
 * Finds an existing section by name or creates a new one.
 * Returns a pointer to the section, or NULL if the section limit
 * has been reached.
 */
static RoseConfigSection *
FindOrAddSection(RoseConfig *cfg, const char *name)
{
   int i;

   /* Search existing */
   for (i = 0; i < cfg->sectionCount; i++) {
      if (strcmp(cfg->sections[i].name, name) == 0) {
         return &cfg->sections[i];
      }
   }

   /* Add new */
   if (cfg->sectionCount >= ROSE_MAX_CONFIG_SECTIONS) {
      return NULL;   /* limit reached */
   }

   strncpy(cfg->sections[cfg->sectionCount].name, name,
           ROSE_CONFIG_STR_MAX - 1);
   cfg->sections[cfg->sectionCount].name[ROSE_CONFIG_STR_MAX - 1] = '\0';
   cfg->sections[cfg->sectionCount].keyCount = 0;
   return &cfg->sections[cfg->sectionCount++];
}


/*
 * AddOrUpdateKey --
 * Adds a key=value pair to a section, or updates it if the key
 * already exists. Silently drops the pair if the key limit is reached.
 */
static void
AddOrUpdateKey(RoseConfigSection *sec, const char *key, const char *value)
{
   int i;

   /* Update existing key */
   for (i = 0; i < sec->keyCount; i++) {
      if (strcmp(sec->keys[i].key, key) == 0) {
         strncpy(sec->keys[i].value, value, ROSE_CONFIG_STR_MAX - 1);
         sec->keys[i].value[ROSE_CONFIG_STR_MAX - 1] = '\0';
         return;
      }
   }

   /* Add new key */
   if (sec->keyCount >= ROSE_MAX_CONFIG_KEYS) {
      return;   /* limit reached - silently drop */
   }

   strncpy(sec->keys[sec->keyCount].key, key, ROSE_CONFIG_STR_MAX - 1);
   sec->keys[sec->keyCount].key[ROSE_CONFIG_STR_MAX - 1] = '\0';
   strncpy(sec->keys[sec->keyCount].value, value, ROSE_CONFIG_STR_MAX - 1);
   sec->keys[sec->keyCount].value[ROSE_CONFIG_STR_MAX - 1] = '\0';
   sec->keyCount++;
}


/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/*
 * RoseConfig_Clear --
 *
 * Resets cfg to empty. Safe to call on an uninitialized struct provided
 * it was zero-initialized (e.g. via memset or static/global declaration).
 */
void
RoseConfig_Clear(RoseConfig *cfg)
{
   if (cfg == NULL) {
      return;
   }
   memset(cfg, 0, sizeof *cfg);
}


/*
 * RoseConfig_Load --
 *
 * Opens path and parses it as an INI file into dst.
 * dst is cleared first. Returns non-zero on success (file opened),
 * zero if the file could not be opened (not an error - tools.conf
 * is optional; all callers should fall back to defaults).
 *
 * Key/value pairs before the first [section] header are placed in
 * an implicit "[vmtools]" section, matching open-vm-tools behaviour.
 */
int
RoseConfig_Load(RoseConfig *dst, const char *path)
{
   FILE              *f;
   char               line[512];   /* generous line buffer */
   char               sectionName[ROSE_CONFIG_STR_MAX];
   char               key[ROSE_CONFIG_STR_MAX];
   char               value[ROSE_CONFIG_STR_MAX];
   RoseConfigSection *currentSection;
   int                lineNum = 0;

   if (dst == NULL || path == NULL) {
      return 0;
   }

   RoseConfig_Clear(dst);

   f = fopen(path, "r");
   if (f == NULL) {
      return 0;   /* file not found - caller uses defaults */
   }

   /*
    * Keys before the first [section] go into an implicit [vmtools]
    * section. Mirrors GKeyFile's default group behaviour.
    */
   strncpy(sectionName, "vmtools", ROSE_CONFIG_STR_MAX - 1);
   sectionName[ROSE_CONFIG_STR_MAX - 1] = '\0';
   currentSection = NULL;   /* created lazily on first key */

   while (fgets(line, sizeof line, f) != NULL) {
      size_t len;
      lineNum++;

      /* Strip trailing newline/CR */
      len = strlen(line);
      while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
         line[--len] = '\0';
      }

      /* Skip blank lines and comments */
      if (IsComment(line)) {
         continue;
      }

      /* Section header? */
      if (ParseSection(line, sectionName)) {
         currentSection = NULL;   /* will be resolved on next key */
         continue;
      }

      /* Key=value pair? */
      if (ParseKeyValue(line, key, value)) {
         if (currentSection == NULL) {
            currentSection = FindOrAddSection(dst, sectionName);
            if (currentSection == NULL) {
               /* Section limit hit - skip remaining keys in this section */
               continue;
            }
         }
         AddOrUpdateKey(currentSection, key, value);
         continue;
      }

      /* Unrecognised line - ignore silently (matches GKeyFile behaviour) */
   }

   fclose(f);
   return 1;
}
