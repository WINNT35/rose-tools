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
 *
 * rose-tools
 * services/vmrosd/pluginMgr.c
 *
 * Plugin lifecycle manager for vmrosd. Mirrors open-vm-tools pluginMgr.c.
 * Handles static plugin loading, two-pass registration, tick dispatch,
 * and reverse-order shutdown.
 *
 * v0.1 scope:
 *   - Static plugin table (no DLL loading)
 *   - Two-pass registration (providers first, apps second)
 *   - Tick dispatch on per-plugin interval
 *   - Reverse-order shutdown
 *
 * TODO v0.2:
 *   - Dynamic plugin loading via LoadLibrary/GetProcAddress
 *   - Plugin directory scanning (FindFirstFile/FindNextFile)
 *   - DLL version checking (ToolsCore_CheckModuleVersion equivalent)
 *   - TOOLS_APP_PROVIDER registration beyond builtins
 *   - State dump functions (RosePluginMgr_DumpState equivalent)
 *   - i18n text domain binding
 *   - AppLoader path
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

/* -----------------------------------------------------------------------
 * Static plugin entry points
 * Add new plugins here as they are implemented.
 * --------------------------------------------------------------------- */
extern RosePluginData *GuestInfo_OnLoad(RoseAppCtx *ctx);

typedef RosePluginData *(*RosePluginEntryFn)(RoseAppCtx *);

static RosePluginEntryFn g_pluginEntries[] = {
   GuestInfo_OnLoad,
   /* TODO v0.2: add new plugins here */
};

#define PLUGIN_COUNT (sizeof(g_pluginEntries) / sizeof(g_pluginEntries[0]))

/* -----------------------------------------------------------------------
 * Internal plugin state
 * --------------------------------------------------------------------- */
typedef struct RosePlugin {
   RosePluginData *data;
   DWORD           lastTick;
} RosePlugin;

static RosePlugin g_plugins[PLUGIN_COUNT];
static int        g_pluginCount = 0;

/* -----------------------------------------------------------------------
 * Built-in provider registration
 *
 * Mirrors the four built-in providers in ToolsCore_RegisterPlugins.
 * Pass 1: registers providers.
 * Pass 2: registers apps (RPC callbacks, signals, properties).
 * --------------------------------------------------------------------- */
static void
RegisterBuiltinProviders(RoseAppCtx *ctx)
{
   /*
    * TODO v0.2: register ROSE_APP_GUESTRPC provider when ctx->rpc != NULL
    * TODO v0.2: register ROSE_APP_SIGNALS provider
    * TODO v0.2: register ROSE_APP_PROVIDER provider
    * TODO v0.2: register ROSE_SVC_PROPERTY provider
    */
   (void)ctx;
}

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
         /*
          * TODO v0.2: RpcChannel_RegisterCallback(ctx->rpc, reg->data)
          */
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
         /* TODO v0.2: ToolsCoreService_RegisterProperty equivalent */
         break;

      default:
         printf("pluginMgr: unknown registration type %d in plugin '%s'.\n",
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
 * RosePluginMgr_Load
 *
 * Loads and initializes all plugins. Mirrors ToolsCore_LoadPlugins +
 * ToolsCore_RegisterPlugins. Returns count of loaded plugins.
 * --------------------------------------------------------------------- */
int
RosePluginMgr_Load(RoseAppCtx *ctx)
{
   int i;
   int loaded = 0;
   RosePluginData *data;

   g_pluginCount = 0;
   memset(g_plugins, 0, sizeof g_plugins);

   /* Pass 1: register built-in providers */
   RegisterBuiltinProviders(ctx);

   /* Load phase: call onload for each plugin */
   for (i = 0; i < (int)PLUGIN_COUNT; i++) {
      data = g_pluginEntries[i](ctx);

      if (data == NULL) {
         printf("pluginMgr: plugin %d declined to load.\n", i);
         continue;
      }

      if (ctx->errorCode != 0) {
         printf("pluginMgr: plugin '%s' requested early exit.\n",
                data->name);
         break;
      }

      /* Init phase */
      if (data->init != NULL) {
         if (!data->init(ctx)) {
            printf("pluginMgr: plugin '%s' init failed.\n", data->name);
            if (data->errorCb != NULL) {
               data->errorCb(ctx, ROSE_APP_GUESTRPC, NULL, data);
            }
            continue;
         }
      }

      g_plugins[g_pluginCount].data     = data;
      g_plugins[g_pluginCount].lastTick = GetTickCount();
      g_pluginCount++;
      loaded++;

      printf("pluginMgr: plugin '%s' loaded.\n", data->name);
   }

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
            printf("pluginMgr: plugin '%s' tick failed.\n",
                   g_plugins[i].data->name);
         }
         g_plugins[i].lastTick = now;
      }
   }
}

/* -----------------------------------------------------------------------
 * RosePluginMgr_Shutdown
 *
 * Shuts down plugins in reverse load order. Mirrors ToolsCore_UnloadPlugins.
 * --------------------------------------------------------------------- */
void
RosePluginMgr_Shutdown(RoseAppCtx *ctx)
{
   int i;

   /* Emit capabilities unregister signal if RPC available */
   if (ctx->rpc != NULL && ctx->signals != NULL) {
      /* TODO v0.2: emit ROSE_SIG_CAPABILITIES with set=FALSE */
   }

   /* Shutdown in reverse order - last loaded first unloaded */
   for (i = g_pluginCount - 1; i >= 0; i--) {
      if (g_plugins[i].data == NULL) {
         continue;
      }
      if (g_plugins[i].data->shutdown != NULL) {
         g_plugins[i].data->shutdown(ctx);
      }
      printf("pluginMgr: plugin '%s' unloaded.\n",
             g_plugins[i].data->name);
      g_plugins[i].data = NULL;
   }

   g_pluginCount = 0;

   /* TODO v0.2: free provider list */
   /* TODO v0.2: FreeLibrary for dynamically loaded plugins */
}
