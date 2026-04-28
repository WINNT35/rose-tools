/*********************************************************
 * Copyright (c) 2026 WINNT35. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *********************************************************/

/**
 * @file rose_plugin.h
 *
 * Defines the interface between vmrosd and its plugins.
 * Architectural equivalent of vmware/tools/plugin.h without GLib.
 *
 * Divergences from upstream (conscious, documented):
 *   - GMainLoop    replaced by Win32 event loop (vmrosd managed)
 *   - GArray       replaced by fixed-size static registration table
 *                  (ROSE_MAX_REGISTRATIONS per plugin)
 *   - GKeyFile     replaced by RoseConfig - INI-style key/value store
 *                  with per-plugin sections matching tools.conf layout
 *   - GObject signals replaced by RoseSignalRegistry with
 *                  RoseRegisterSignal / RoseEmitSignal API
 *   - gboolean     -> int
 *   - gchar        -> char
 *   - gpointer     -> void *
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */
 
#ifndef _ROSE_PLUGIN_H_
#define _ROSE_PLUGIN_H_ 

#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "vm_basic_types.h"
#include "rpcout.h"
#include "vmware/tools/guestrpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Limits
 * --------------------------------------------------------------------- */

/** Maximum number of feature registrations per plugin. */
#define ROSE_MAX_REGISTRATIONS   32

/** Maximum number of signal handlers system-wide. */
#define ROSE_MAX_SIGNAL_HANDLERS 64

/** Maximum key/value pairs per config section. */
#define ROSE_MAX_CONFIG_KEYS     64

/** Maximum config sections (one per plugin + one global). */
#define ROSE_MAX_CONFIG_SECTIONS 32

/** Maximum length of a config key, value, or section name string. */
#define ROSE_CONFIG_STR_MAX      256

/* -----------------------------------------------------------------------
 * API versioning  (mirrors ToolsCoreAPI)
 * --------------------------------------------------------------------- */
typedef enum {
   ROSE_CORE_API_V1 = 0x1
} RoseCoreAPI;

/* -----------------------------------------------------------------------
 * Signal names  (mirrors TOOLS_CORE_SIG_* constants)
 * --------------------------------------------------------------------- */
#define ROSE_SIG_CAPABILITIES    "rose_capabilities"
#define ROSE_SIG_RESET           "rose_reset"
#define ROSE_SIG_NO_RPC          "rose_no_rpc"
#define ROSE_SIG_SET_OPTION      "rose_set_option"
#define ROSE_SIG_PRE_SHUTDOWN    "rose_pre_shutdown"
#define ROSE_SIG_SHUTDOWN        "rose_shutdown"
#define ROSE_SIG_DUMP_STATE      "rose_dump_state"
#define ROSE_SIG_CONF_RELOAD     "rose_conf_reload"
#define ROSE_SIG_SERVICE_CONTROL "rose_service_control"

/* -----------------------------------------------------------------------
 * Config  (replaces GKeyFile)
 *
 * INI-style config matching tools.conf layout:
 *   [vmtools]        <- global section
 *   key=value
 *
 *   [heartbeat]      <- per-plugin section
 *   key=value
 * --------------------------------------------------------------------- */
typedef struct RoseConfigKey {
   char key[ROSE_CONFIG_STR_MAX];
   char value[ROSE_CONFIG_STR_MAX];
} RoseConfigKey;

typedef struct RoseConfigSection {
   char          name[ROSE_CONFIG_STR_MAX];
   RoseConfigKey keys[ROSE_MAX_CONFIG_KEYS];
   int           keyCount;
} RoseConfigSection;

typedef struct RoseConfig {
   RoseConfigSection sections[ROSE_MAX_CONFIG_SECTIONS];
   int               sectionCount;
} RoseConfig;

/**
 * Look up a value in the config by section and key.
 * Returns the value string or NULL if not found.
 * Mirrors g_key_file_get_string().
 */
static const char *
RoseConfig_GetString(const RoseConfig *cfg,
                     const char *section,
                     const char *key)
{
   int i;
   int j;

   if (cfg == NULL || section == NULL || key == NULL) {
      return NULL;
   }
   for (i = 0; i < cfg->sectionCount; i++) {
      if (strcmp(cfg->sections[i].name, section) == 0) {
         for (j = 0; j < cfg->sections[i].keyCount; j++) {
            if (strcmp(cfg->sections[i].keys[j].key, key) == 0) {
               return cfg->sections[i].keys[j].value;
            }
         }
      }
   }
   return NULL;
}

/**
 * Look up an integer value in the config.
 * Returns defVal if not found or not parseable.
 * Mirrors g_key_file_get_integer().
 */
static int
RoseConfig_GetInt(const RoseConfig *cfg,
                  const char *section,
                  const char *key,
                  int defVal)
{
   const char *val = RoseConfig_GetString(cfg, section, key);
   if (val == NULL) {
      return defVal;
   }
   return atoi(val);
}

/**
 * Look up a boolean value in the config.
 * Accepts "true"/"false" and "1"/"0".
 * Returns defVal if not found.
 * Mirrors g_key_file_get_boolean().
 */
static int
RoseConfig_GetBool(const RoseConfig *cfg,
                   const char *section,
                   const char *key,
                   int defVal)
{
   const char *val = RoseConfig_GetString(cfg, section, key);
   if (val == NULL) {
      return defVal;
   }
   if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
      return 1;
   }
   if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0) {
      return 0;
   }
   return defVal;
}

/* -----------------------------------------------------------------------
 * Application context  (mirrors ToolsAppCtx)
 * --------------------------------------------------------------------- */
 /* Forward declaration - defined below */
typedef struct RoseSignalRegistry RoseSignalRegistry;
typedef struct RoseAppCtx {
   /** Supported API versions. Bitmask. */
   RoseCoreAPI        version;
   /** Name of the service. */
   const char        *name;
   /** Whether running under a VMware hypervisor. */
   int                isVMware;
   /** Error code. Non-zero causes vmrosd main loop to exit. */
   int                errorCode;
   /** Whether the main loop should keep running. */
   int                running;
   /** Whether COM is initialized. */
   int                comInitialized;
   /** INI-style config. Loaded from tools.conf at startup. */
   RoseConfig        *config;
   /** Signal dispatch table. Plugins call RoseRegisterSignal(ctx->signals, ...) */
   RoseSignalRegistry *signals;
   /** RPC channel for guest<->host communication. */
   RpcChannel        *rpc;
} RoseAppCtx;
/* -----------------------------------------------------------------------
 * Signal dispatch  (replaces GObject signal bus)
 * --------------------------------------------------------------------- */

/**
 * Signature for a signal handler callback.
 * src  - the object that emitted the signal (usually RoseAppCtx *)
 * data - client data registered with the handler
 */
typedef void (*RoseSignalFunc)(void *src, void *data);

typedef struct RoseSignalHandler {
   char           signame[ROSE_CONFIG_STR_MAX];
   RoseSignalFunc callback;
   void          *clientData;
} RoseSignalHandler;

/**
 * Signal registry. Owned by vmrosd, passed to plugins via RoseAppCtx.
 * Plugins call RoseRegisterSignal to subscribe.
 * vmrosd calls RoseEmitSignal to dispatch.
 */
typedef struct RoseSignalRegistry {
   RoseSignalHandler handlers[ROSE_MAX_SIGNAL_HANDLERS];
   int               count;
} RoseSignalRegistry;

/**
 * Register a signal handler.
 * Mirrors g_signal_connect().
 * Returns non-zero on success, zero if the registry is full.
 */
static int
RoseRegisterSignal(RoseSignalRegistry *reg,
                   const char *signame,
                   RoseSignalFunc callback,
                   void *clientData)
{
   if (reg == NULL || signame == NULL || callback == NULL) {
      return 0;
   }
   if (reg->count >= ROSE_MAX_SIGNAL_HANDLERS) {
      return 0;
   }
   strncpy(reg->handlers[reg->count].signame,
           signame, ROSE_CONFIG_STR_MAX - 1);
   reg->handlers[reg->count].signame[ROSE_CONFIG_STR_MAX - 1] = '\0';
   reg->handlers[reg->count].callback   = callback;
   reg->handlers[reg->count].clientData = clientData;
   reg->count++;
   return 1;
}

/**
 * Emit a signal, calling all registered handlers for that signal name.
 * Mirrors g_signal_emit_by_name().
 */
static void
RoseEmitSignal(RoseSignalRegistry *reg,
               const char *signame,
               void *src)
{
   int i;
   if (reg == NULL || signame == NULL) {
      return;
   }
   for (i = 0; i < reg->count; i++) {
      if (strcmp(reg->handlers[i].signame, signame) == 0) {
         reg->handlers[i].callback(src, reg->handlers[i].clientData);
      }
   }
}

/* -----------------------------------------------------------------------
 * Capability types  (mirrors ToolsCapabilityType)
 * --------------------------------------------------------------------- */
typedef enum {
   ROSE_CAP_OLD       = 0,
   ROSE_CAP_OLD_NOVAL = 1,
   ROSE_CAP_NEW       = 2
} RoseCapabilityType;

/* -----------------------------------------------------------------------
 * Capability descriptor  (mirrors ToolsAppCapability)
 * --------------------------------------------------------------------- */
typedef struct RoseAppCapability {
   RoseCapabilityType  type;
   const char         *name;
   unsigned int        index;
   unsigned int        value;
} RoseAppCapability;

/* -----------------------------------------------------------------------
 * Feature registration types  (mirrors ToolsAppType)
 * --------------------------------------------------------------------- */
typedef enum {
   ROSE_APP_GUESTRPC  = 1,   /* RPC command handler */
   ROSE_APP_SIGNALS   = 2,   /* Signal subscription  */
   ROSE_APP_PROVIDER  = 3,   /* Application provider */
   ROSE_SVC_PROPERTY  = 4    /* Service property     */
} RoseAppType;

/* -----------------------------------------------------------------------
 * Feature registration entry  (mirrors ToolsAppReg)
 * --------------------------------------------------------------------- */
typedef struct RoseAppReg {
   RoseAppType   type;
   void         *data;
} RoseAppReg;

/* -----------------------------------------------------------------------
 * Service property  (mirrors ToolsServiceProperty)
 * --------------------------------------------------------------------- */
typedef struct RoseServiceProperty {
   const char *name;
} RoseServiceProperty;

/* -----------------------------------------------------------------------
 * Signal callback descriptor  (mirrors ToolsPluginSignalCb)
 * --------------------------------------------------------------------- */
typedef struct RosePluginSignalCb {
   const char  *signame;
   void        *callback;
   void        *clientData;
} RosePluginSignalCb;

/* -----------------------------------------------------------------------
 * Plugin registration data  (mirrors ToolsPluginData)
 * --------------------------------------------------------------------- */
typedef struct RosePluginData {
   /** Plugin name (required). */
   const char    *name;
   /**
    * Feature registrations. Replaces GArray *regs.
    * Supports ROSE_APP_GUESTRPC, ROSE_APP_SIGNALS, ROSE_APP_PROVIDER,
    * ROSE_SVC_PROPERTY in the same order as upstream ToolsPluginData.
    */
   RoseAppReg     regs[ROSE_MAX_REGISTRATIONS];
   int            regCount;
   /**
    * Called once on startup after vmrosd establishes the RPC channel.
    * Return non-zero on success, zero to abort plugin load.
    */
   int (*init)(RoseAppCtx *ctx);
   /**
    * Called periodically by vmrosd on the plugin's tick interval.
    * Return non-zero to keep running, zero to signal an error.
    */
   int (*tick)(RoseAppCtx *ctx);
   /**
    * Called once on shutdown. May be NULL if no cleanup needed.
    */
   void (*shutdown)(RoseAppCtx *ctx);
   /**
    * Registration error callback. Mirrors ToolsPluginData.errorCb.
    * Called when a registration entry fails.
    * Return non-zero to continue, zero to stop registration.
    */
   int (*errorCb)(RoseAppCtx *ctx,
                  RoseAppType type,
                  void *data,
                  struct RosePluginData *plugin);
   /** Tick interval in milliseconds. */
   DWORD          tickIntervalMs;
   /** Private plugin data. Managed by the plugin. */
   void          *_private;
} RosePluginData;

/* -----------------------------------------------------------------------
 * Plugin entry point  (mirrors ToolsPluginOnLoad)
 * --------------------------------------------------------------------- */
typedef RosePluginData *(*RosePluginOnLoad)(RoseAppCtx *ctx);

/* -----------------------------------------------------------------------
 * Service name constants
 * --------------------------------------------------------------------- */
#define ROSE_GUEST_SERVICE  "vmrosd"
#define ROSE_USER_SERVICE   "vmrosd-user"

/* -----------------------------------------------------------------------
 * Convenience macros
 * --------------------------------------------------------------------- */
#define ROSE_IS_MAIN_SERVICE(ctx) (strcmp((ctx)->name, ROSE_GUEST_SERVICE) == 0)
#define ROSE_IS_USER_SERVICE(ctx) (strcmp((ctx)->name, ROSE_USER_SERVICE) == 0)

/**
 * Signal an error and stop the main loop.
 * Mirrors VMTOOLSAPP_ERROR.
 */
#define ROSEAPP_ERROR(ctx, err) do { \
   (ctx)->errorCode = (err);         \
   (ctx)->running   = 0;             \
} while (0)

/* -----------------------------------------------------------------------
 * Plugin export tag  (mirrors TOOLS_MODULE_EXPORT)
 * --------------------------------------------------------------------- */
#define ROSE_MODULE_EXPORT __declspec(dllexport)

#ifdef __cplusplus
}
#endif

#endif /* _ROSE_PLUGIN_H_ */
