/*********************************************************
 * Copyright (C) 2008-2020 VMware, Inc. All rights reserved.
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
 * @file pluginMgr.c
 *
 * Plugin lifecycle manager for vmrosd.
 * Mirrors open-vm-tools pluginMgr.c / ToolsCore_LoadPlugins.
 *
 * v0.2 changes from v0.1:
 *   - Dynamic plugin loading via LoadLibrary / GetProcAddress
 *     replaces the static plugin table.
 *   - Plugin directory scanned with FindFirstFile / FindNextFile,
 *     entries loaded in alphabetical order (mirrors GDir sort).
 *   - Plugin directory read from config key [vmtools] plugin_dir;
 *     falls back to ROSE_DEFAULT_PLUGIN_DIR if absent.
 *   - Entry point looked up by name ROSE_PLUGIN_ENTRY ("RoseOnLoad"),
 *     mirrors upstream "ToolsOnLoad".
 *   - FreeLibrary called on shutdown for dynamically loaded plugins.
 *
 * Divergences from upstream (conscious, documented):
 *   - GLib (GPtrArray, GModule, g_dir_open) replaced by Win32 API
 *     (HeapAlloc / HeapFree, LoadLibrary, FindFirstFile).
 *   - ToolsCore_CheckModuleVersion not yet implemented (TODO v0.3).
 *   - AppLoader path not implemented (not needed on Windows).
 *   - Two-pass provider/app registration retained from v0.1;
 *     ROSE_APP_PROVIDER registration beyond builtins is TODO v0.3.
 *   - State dump (DumpPluginInfo) is TODO v0.3.
 *   - i18n text domain binding not applicable (no GLib i18n).
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"
#include "roseConfig.h"

/* -----------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------- */

/* Default plugin directory when [vmtools] plugin_dir is not set */
#define ROSE_DEFAULT_PLUGIN_DIR  "C:\\Program Files\\VMware\\Rose Tools\\plugins\\vmsvc"

/* Exported entry point name every plugin DLL must provide */
#define ROSE_PLUGIN_ENTRY        "RoseOnLoad"

/* Maximum number of plugins that can be loaded simultaneously */
#define ROSE_MAX_PLUGINS         64

/* -----------------------------------------------------------------------
 * Internal plugin state
 * --------------------------------------------------------------------- */
typedef struct RosePlugin {
   char            name[MAX_PATH];   /* plugin DLL filename (for logging)  */
   HMODULE         module;           /* NULL for statically linked plugins  */
   RosePluginData *data;             /* returned by RoseOnLoad              */
   DWORD           lastTick;         /* last tick timestamp (ms)            */
} RosePlugin;

static RosePlugin g_plugins[ROSE_MAX_PLUGINS];
static int        g_pluginCount = 0;

/* -----------------------------------------------------------------------
 * Simple dynamic array for collecting DLL filenames before sorting.
 * Mirrors upstream GPtrArray usage in ToolsCoreLoadDirectory.
 * --------------------------------------------------------------------- */
#define ROSE_MAX_DIR_ENTRIES  256

typedef struct RoseDirList {
   char entries[ROSE_MAX_DIR_ENTRIES][MAX_PATH];
   int  count;
} RoseDirList;

/* -----------------------------------------------------------------------
 * StrCompare
 *
 * Comparison function for qsort - sorts DLL filenames alphabetically.
 * Mirrors ToolsCoreStrPtrCompare.
 * --------------------------------------------------------------------- */
static int
StrCompare(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}


/* -----------------------------------------------------------------------
 * RegisterPluginApps
 *
 * Pass 2: registers RPC callbacks, signals, and properties for a plugin.
 * Retained from v0.1.
 * --------------------------------------------------------------------- */
static void
RegisterPluginApps(RoseAppCtx *ctx, RosePlugin *plugin)
{
   int i;
   RoseAppReg *reg;

   if (plugin->data->regCount == 0) {
      return;
   }

   for (i = 0; i < plugin->data->regCount; i++) {
      reg = &plugin->data->regs[i];

      switch (reg->type) {
      case ROSE_APP_GUESTRPC:
         /* TODO v0.3: RpcChannel_RegisterCallback(ctx->rpc, reg->data) */
         (void)ctx;
         break;

      case ROSE_APP_SIGNALS:
         if (ctx->signals != NULL && reg->data != NULL) {
            RosePluginSignalCb *cb = (RosePluginSignalCb *)reg->data;
            RoseRegisterSignal(ctx->signals,
                               cb->signame,
                               (RoseSignalFunc)cb->callback,
                               cb->clientData);
         }
         break;

      case ROSE_APP_PROVIDER:
         /* Handled in pass 1, skip in pass 2 */
         break;

      case ROSE_SVC_PROPERTY:
         /* TODO v0.3: ToolsCoreService_RegisterProperty equivalent */
         break;

      default:
         fprintf(stderr,
                 "pluginMgr: unknown registration type %d in plugin '%s'.\n",
                 reg->type, plugin->data->name);
         if (plugin->data->errorCb != NULL) {
            if (!plugin->data->errorCb(ctx, reg->type, reg->data,
                                       plugin->data)) {
               return;
            }
         }
         break;
      }
   }
}


/* -----------------------------------------------------------------------
 * LoadPluginFromPath
 *
 * Loads a single DLL, resolves RoseOnLoad, calls it, then calls init().
 * On success adds the plugin to g_plugins and returns non-zero.
 * Mirrors the inner loop of ToolsCoreLoadDirectory +
 * the onload/init block of ToolsCore_LoadPlugins.
 * --------------------------------------------------------------------- */
static int
LoadPluginFromPath(RoseAppCtx *ctx, const char *path, const char *filename)
{
   HMODULE         module;
   RoseOnLoadFn    onload;
   RosePluginData *data;

   if (g_pluginCount >= ROSE_MAX_PLUGINS) {
      fprintf(stderr, "pluginMgr: plugin limit (%d) reached, skipping '%s'.\n",
              ROSE_MAX_PLUGINS, filename);
      return 0;
   }

   module = LoadLibraryA(path);
   if (module == NULL) {
      fprintf(stderr, "pluginMgr: LoadLibrary failed for '%s' (err=%lu).\n",
              filename, GetLastError());
      return 0;
   }

   /* Mirrors: g_module_symbol(module, "ToolsOnLoad", &onload) */
   onload = (RoseOnLoadFn)(void *)GetProcAddress(module, ROSE_PLUGIN_ENTRY);
   if (onload == NULL) {
      fprintf(stderr,
              "pluginMgr: '%s' does not export %s, skipping.\n",
              filename, ROSE_PLUGIN_ENTRY);
      FreeLibrary(module);
      return 0;
   }

   /* Mirrors: plugin->data = plugin->onload(&state->ctx) */
   data = onload(ctx);
   if (data == NULL) {
      fprintf(stderr, "pluginMgr: '%s' OnLoad returned NULL, skipping.\n",
              filename);
      FreeLibrary(module);
      return 0;
   }

   if (ctx->errorCode != 0) {
      fprintf(stderr, "pluginMgr: '%s' requested early exit.\n", filename);
      FreeLibrary(module);
      return 0;
   }

   /* Call init if provided */
   if (data->init != NULL) {
      if (!data->init(ctx)) {
         fprintf(stderr, "pluginMgr: plugin '%s' init() failed.\n",
                 data->name);
         if (data->errorCb != NULL) {
            data->errorCb(ctx, ROSE_APP_GUESTRPC, NULL, data);
         }
         FreeLibrary(module);
         return 0;
      }
   }

   /* Commit to the loaded list */
   strncpy(g_plugins[g_pluginCount].name, filename, MAX_PATH - 1);
   g_plugins[g_pluginCount].name[MAX_PATH - 1] = '\0';
   g_plugins[g_pluginCount].module   = module;
   g_plugins[g_pluginCount].data     = data;
   g_plugins[g_pluginCount].lastTick = GetTickCount();
   g_pluginCount++;

   fprintf(stdout, "pluginMgr: plugin '%s' loaded.\n", data->name);
   return 1;
}


/* -----------------------------------------------------------------------
 * LoadPluginDirectory
 *
 * Scans pluginDir for *.dll files, sorts them alphabetically, then
 * loads each one via LoadPluginFromPath.
 * Mirrors ToolsCoreLoadDirectory.
 * Returns number of plugins successfully loaded from this directory.
 * --------------------------------------------------------------------- */
static int
LoadPluginDirectory(RoseAppCtx *ctx, const char *pluginDir)
{
   RoseDirList     list;
   WIN32_FIND_DATA fd;
   HANDLE          hFind;
   char            pattern[MAX_PATH];
   char            fullPath[MAX_PATH];
   int             i;
   int             loaded = 0;

   /* Check directory exists */
   if (GetFileAttributesA(pluginDir) == INVALID_FILE_ATTRIBUTES) {
      fprintf(stderr, "pluginMgr: plugin directory not found: %s\n",
              pluginDir);
      return 0;
   }

   /* Build wildcard pattern */
   _snprintf(pattern, sizeof pattern, "%s\\*.dll", pluginDir);
   pattern[sizeof pattern - 1] = '\0';

   memset(&list, 0, sizeof list);

   /* Collect all .dll filenames - mirrors g_dir_read_name loop */
   hFind = FindFirstFileA(pattern, &fd);
   if (hFind == INVALID_HANDLE_VALUE) {
      /* No DLLs found - empty directory is not an error */
      return 0;
   }

   do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
         continue;
      }
      if (list.count >= ROSE_MAX_DIR_ENTRIES) {
         fprintf(stderr, "pluginMgr: too many DLLs in '%s', truncating.\n",
                 pluginDir);
         break;
      }
      strncpy(list.entries[list.count], fd.cFileName, MAX_PATH - 1);
      list.entries[list.count][MAX_PATH - 1] = '\0';
      list.count++;
   } while (FindNextFileA(hFind, &fd));

   FindClose(hFind);

   /* Sort alphabetically - mirrors g_ptr_array_sort */
   qsort(list.entries, (size_t)list.count,
         sizeof list.entries[0], StrCompare);

   /* Load each DLL in sorted order */
   for (i = 0; i < list.count; i++) {
      _snprintf(fullPath, sizeof fullPath, "%s\\%s",
                pluginDir, list.entries[i]);
      fullPath[sizeof fullPath - 1] = '\0';
      loaded += LoadPluginFromPath(ctx, fullPath, list.entries[i]);

      /* Break early if a plugin requested exit */
      if (ctx->errorCode != 0) {
         break;
      }
   }

   return loaded;
}


/* -----------------------------------------------------------------------
 * RosePluginMgr_Load
 *
 * Resolves the plugin directory, scans and loads all DLLs, then runs
 * the two-pass registration.
 * Mirrors ToolsCore_LoadPlugins + ToolsCore_RegisterPlugins.
 * Returns count of loaded plugins.
 * --------------------------------------------------------------------- */
int
RosePluginMgr_Load(RoseAppCtx *ctx)
{
   const char *pluginDir;
   int         i;
   int         loaded = 0;

   g_pluginCount = 0;
   memset(g_plugins, 0, sizeof g_plugins);

   /* Resolve plugin directory from config, fall back to default */
   pluginDir = NULL;
   if (ctx->config != NULL) {
      pluginDir = RoseConfig_GetString(ctx->config, "vmtools", "plugin_dir");
   }
   if (pluginDir == NULL || pluginDir[0] == '\0') {
      pluginDir = ROSE_DEFAULT_PLUGIN_DIR;
   }

   fprintf(stdout, "pluginMgr: loading plugins from: %s\n", pluginDir);

   loaded = LoadPluginDirectory(ctx, pluginDir);

   /* Pass 2: register apps (RPC callbacks, signals, properties) */
   for (i = 0; i < g_pluginCount; i++) {
      RegisterPluginApps(ctx, &g_plugins[i]);
   }

   return loaded;
}


/* -----------------------------------------------------------------------
 * RosePluginMgr_Tick
 *
 * Calls each plugin's tick function on its configured interval.
 * Called from the main loop in mainLoop.c.
 * --------------------------------------------------------------------- */
void
RosePluginMgr_Tick(RoseAppCtx *ctx)
{
   int   i;
   DWORD now = GetTickCount();

   for (i = 0; i < g_pluginCount; i++) {
      if (g_plugins[i].data == NULL) {
         continue;
      }
      if (g_plugins[i].data->tick == NULL) {
         continue;
      }
      if (g_plugins[i].data->tickIntervalMs == 0) {
         continue;
      }
      if ((now - g_plugins[i].lastTick) >= g_plugins[i].data->tickIntervalMs) {
         if (!g_plugins[i].data->tick(ctx)) {
            fprintf(stderr, "pluginMgr: plugin '%s' tick failed.\n",
                    g_plugins[i].data->name);
         }
         g_plugins[i].lastTick = now;
      }
   }
}


/* -----------------------------------------------------------------------
 * RosePluginMgr_Shutdown
 *
 * Calls each plugin's shutdown callback in reverse load order.
 * Does NOT call FreeLibrary - that is deferred to RosePluginMgr_Free
 * which must be called AFTER RoseToolsRpc_Shutdown so the TCLO channel
 * is closed cleanly before any DLL memory is released.
 * Mirrors ToolsCore_UnloadPlugins.
 * --------------------------------------------------------------------- */
void
RosePluginMgr_Shutdown(RoseAppCtx *ctx)
{
   int i;

   /* Emit capabilities unregister signal if RPC available */
   if (ctx->rpc != NULL && ctx->signals != NULL) {
      /* TODO v0.3: emit ROSE_SIG_CAPABILITIES with set=FALSE */
   }

   /* Shutdown callbacks in reverse order - last loaded, first unloaded.
    * Mirrors the reverse iteration in ToolsCore_UnloadPlugins. */
   for (i = g_pluginCount - 1; i >= 0; i--) {
      if (g_plugins[i].data == NULL) {
         continue;
      }

      if (g_plugins[i].data->shutdown != NULL) {
         g_plugins[i].data->shutdown(ctx);
      }

      fprintf(stdout, "pluginMgr: plugin '%s' unloaded.\n",
              g_plugins[i].data->name);

      g_plugins[i].data = NULL;
   }

   /* TODO v0.3: free provider list */
}


/* -----------------------------------------------------------------------
 * RosePluginMgr_Free
 *
 * Releases DLL handles. Must be called AFTER RoseToolsRpc_Shutdown
 * to ensure the TCLO channel is closed before DLL memory is freed.
 * Calling FreeLibrary before RPC shutdown risks crashing if VMware
 * sends a final TCLO command whose handler lives in the unloaded DLL.
 * --------------------------------------------------------------------- */
void
RosePluginMgr_Free(void)
{
   int i;

   for (i = g_pluginCount - 1; i >= 0; i--) {
      if (g_plugins[i].module != NULL) {
         FreeLibrary(g_plugins[i].module);
         g_plugins[i].module = NULL;
      }
   }

   g_pluginCount = 0;
}
