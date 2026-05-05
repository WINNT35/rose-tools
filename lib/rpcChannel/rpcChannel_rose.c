/*********************************************************
 * Copyright (c) 2008-2016, 2018-2021, 2023 VMware, Inc. All rights reserved.
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
 * @file rpcChannel_rose.c
 *
 *    Common functions to all RPC channel implementations.
 *
 *    Derived from open-vm-tools rpcChannel.c. This file replaces
 *    rpcChannel.c for the rose-tools Win32/C89/backdoor-only build.
 *
 *    Changes from upstream (all marked ROSE-TOOLS BEGIN/END):
 *
 *    1. USE_RPCI_ONLY is always defined. NEED_RPCIN blocks are
 *       entirely absent - inbound RPC is handled by rpcin.c directly,
 *       wired through toolsRpc.c via RpcIn_Poll.
 *
 *    2. GLib runtime replaced by inline stubs, derived from
 *       open-vm-tools lib/rpcChannel/glib_stubs.c (c) 2018-2020 VMware.
 *       glib.h header dependency eliminated by defining the required
 *       GLib types inline (same approach as guestrpc.h).
 *
 *    3. vSocket / mutable channel code (#if _WIN32 blocks in upstream)
 *       commented out. BackdoorChannel_New() is always used.
 *
 *    4. rpcChannelInt.h reconstructed inline - that header is not
 *       shipped in the open-vm-tools source distribution.
 *
 *    5. Util_SafeStrdup replaced with plain strdup.
 *       strutil.h / util.h not in tree.
 *
 *    Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

/* ROSE-TOOLS BEGIN: build configuration.
 * USE_RPCI_ONLY suppresses NEED_RPCIN throughout - we handle inbound
 * separately via rpcin.c. This matches glib_stubs.c's requirement.
 * ROSE-TOOLS ORIGINAL: these were set externally via build flags. */
#define USE_RPCI_ONLY
/* ROSE-TOOLS END */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ROSE-TOOLS BEGIN: GLib type shims + runtime stubs.
 * Derived from open-vm-tools lib/rpcChannel/glib_stubs.c
 * Copyright (C) 2018-2020 VMware, Inc.
 *
 * glib_stubs.c has:
 *   #include <glib.h>        -- eliminated, types defined below instead
 *   #include "util.h"        -- eliminated, Util_SafeCalloc -> calloc
 *
 * ROSE-TOOLS ORIGINAL (glib_stubs.c):
 *   #include <glib.h>
 *   #include "util.h"
 *   void *g_malloc0(size_t s)           { return Util_SafeCalloc(1, s); }
 *   void *g_malloc0_n(size_t n, size_t s){ return Util_SafeCalloc(n, s); }
 *   void  g_free(void *p)               { free(p); }
 *   void  g_mutex_init(GMutex *mutex)   { }
 *   void  g_mutex_clear(GMutex *mutex)  { }
 *   void  g_mutex_lock(GMutex *mutex)   { }
 *   void  g_mutex_unlock(GMutex *mutex) { }
 *   void  g_usleep(gulong microseconds) { }
 */

/* GLib primitive types - same shim pattern as guestrpc.h */
#ifndef ROSE_GLIB_SHIM
#define ROSE_GLIB_SHIM
typedef int          gboolean;
typedef char         gchar;
typedef void        *gpointer;
typedef unsigned int guint;
#ifndef TRUE
#  define TRUE  1
#endif
#ifndef FALSE
#  define FALSE 0
#endif
#endif /* ROSE_GLIB_SHIM */

typedef unsigned long gulong;

/* GMutex stub - single-threaded build, no locking needed. */
typedef struct { int _unused; } GMutex;

/* g_new0: typed calloc wrapper used in RpcChannel_Create. */
#define g_new0(type, n)  ((type *)calloc((n), sizeof(type)))

/* GLib runtime stubs (from glib_stubs.c, Util_SafeCalloc -> calloc) */
static void *g_malloc0(size_t s)              { return calloc(1, s); }
static void *g_malloc0_n(size_t n, size_t s)  { return calloc(n, s); }
static void  g_free(void *p)                  { free(p); }
static void  g_mutex_init(GMutex *m)          { (void)m; }
static void  g_mutex_clear(GMutex *m)         { (void)m; }
static void  g_mutex_lock(GMutex *m)          { (void)m; }
static void  g_mutex_unlock(GMutex *m)        { (void)m; }

/* Suppress unused-function warnings for stubs not referenced in this path */
static void  rose_suppress_stubs(void) {
   (void)g_malloc0; (void)g_malloc0_n; (void)g_free;
   (void)g_mutex_init; (void)g_mutex_clear;
   (void)g_mutex_lock; (void)g_mutex_unlock;
}
/* ROSE-TOOLS END */

#include "debug.h"

/* ROSE-TOOLS BEGIN: includes needed before rpcChannelInt reconstruction.
 * guestrpc.h provides: typedef struct _RpcChannel RpcChannel,
 *   RpcChannelType enum, gboolean, RpcInData, RpcChannelCallback etc.
 * vmware.h provides: Bool.
 * vm_assert.h provides: ASSERT.
 * Must precede RpcChannelFuncs which uses RpcChannel* and RpcChannelType.
 * ROSE-TOOLS ORIGINAL: all in "remaining headers" block below. */
#include "vmware.h"
#include "vm_assert.h"
#include "str.h"
#include "vmware/tools/guestrpc.h"
#include "rpcout.h"
/* ROSE-TOOLS END */

/* ROSE-TOOLS BEGIN: rpcChannelInt.h reconstructed inline.
 * This header is not in the open-vm-tools source distribution.
 * Reconstructed from field accesses in rpcChannel.c.
 *
 * Under USE_RPCI_ONLY the RpcChannelInt struct has no NEED_RPCIN fields,
 * so it is simply RpcChannel with the outLock mutex appended.
 *
 * RpcChannelFuncs vtable reconstructed from funcs-> accesses:
 *   start, stop, send, setup, shutdown, getType, destroy.
 * 'setup' and 'shutdown' are only used inside NEED_RPCIN blocks (which we
 * don't compile), so they're present for ABI completeness only.
 *
 * RpcChannel base struct fields used outside NEED_RPCIN:
 *   funcs, outStarted, outLock, isMutable, vsockFailureTS,
 *   vsockRetryDelay, vsockChannelFlags, in, inStarted, mainCtx.
 * Under USE_RPCI_ONLY + backdoor-only: in/inStarted/mainCtx/
 * vsock* fields exist but are always zero/NULL.
 *
 * ROSE-TOOLS ORIGINAL: #include "rpcChannelInt.h"
 */

/* ROSE-TOOLS BEGIN: forward decl removed - guestrpc.h provides
 *   typedef struct _RpcChannel RpcChannel;
 * We must use struct _RpcChannel (not struct RpcChannel) as the tag
 * to complete the type declared there.
 * ROSE-TOOLS ORIGINAL: typedef struct RpcChannel RpcChannel; */
/* ROSE-TOOLS END */

/* ROSE-TOOLS BEGIN: forward declaration needed for mutual reference:
 * RpcChannelFuncs uses RpcChannel*, struct _RpcChannel uses RpcChannelFuncs*.
 * ROSE-TOOLS ORIGINAL: rpcChannelInt.h resolved this via include order. */
typedef struct RpcChannelFuncs RpcChannelFuncs;
/* ROSE-TOOLS END */

/* Vtable for a channel implementation. */
struct RpcChannelFuncs {
   gboolean (*start)(RpcChannel *chan);
   void     (*stop)(RpcChannel *chan);
   gboolean (*send)(RpcChannel *chan,
                    char const *data, size_t dataLen,
                    Bool *rpcStatus,
                    char **result, size_t *resultLen);
   void     (*setup)(RpcChannel *chan,
                     void *mainCtx,        /* GMainContext* in upstream */
                     const char *appName,
                     gpointer appCtx);
   void     (*shutdown)(RpcChannel *chan);
   RpcChannelType (*getType)(RpcChannel *chan);
   void     (*destroy)(RpcChannel *chan);
}; /* RpcChannelFuncs */

/* Base channel struct. Completes the forward decl in guestrpc.h.
 * ROSE-TOOLS BEGIN: tag changed RpcChannel -> _RpcChannel to match
 *   typedef struct _RpcChannel RpcChannel in guestrpc.h.
 * ROSE-TOOLS ORIGINAL: struct RpcChannel { */
struct _RpcChannel {
   const RpcChannelFuncs *funcs;

   /* Outbound state */
   GMutex   outLock;       /* guards outStarted and Send calls */
   gboolean outStarted;

   /* Inbound state - NULL/FALSE in USE_RPCI_ONLY build */
   void    *in;            /* RpcIn* in upstream; unused here */
   gboolean inStarted;
   void    *mainCtx;       /* GMainContext* in upstream; unused here */

   /* vSocket fallback tracking - always zero in backdoor-only build */
   gboolean isMutable;
   time_t   vsockFailureTS;
   unsigned int vsockRetryDelay;
   int      vsockChannelFlags;
}; /* end struct _RpcChannel */

/* Internal channel state - no NEED_RPCIN fields under USE_RPCI_ONLY. */
typedef struct RpcChannelInt {
   RpcChannel impl;        /* must be first */
} RpcChannelInt;

/* ROSE-TOOLS END */  /* end rpcChannelInt.h reconstruction */

/* ROSE-TOOLS BEGIN: remaining headers not in tree replaced/dropped.
 * All necessary includes moved before rpcChannelInt reconstruction above.
 * ROSE-TOOLS ORIGINAL:
 *   #include "rpcChannelInt.h"   -- reconstructed above
 *   #include "dynxdr.h"          -- NEED_RPCIN only, not compiled
 *   #include "vmxrpc.h"          -- NEED_RPCIN only, not compiled
 *   #include "xdrutil.h"         -- NEED_RPCIN only, not compiled
 *   #include "rpcin.h"           -- NEED_RPCIN only, not compiled here
 *   #include "vmware/guestrpc/tclodefs.h" -- NEED_RPCIN only
 *   #include "strutil.h"         -- StrUtil_GetNextToken in rpcin.c shim
 *   #include "util.h"            -- Util_SafeStrdup -> strdup below
 *   #include "vmware.h"          -- moved above
 *   #include "vm_assert.h"       -- moved above
 *   #include "str.h"             -- moved above
 *   #include "vmware/tools/guestrpc.h" -- moved above
 *   #include "rpcout.h"          -- moved above
 */
/* ROSE-TOOLS END */

/* ROSE-TOOLS BEGIN: Util_SafeStrdup not available without util.h.
 * ROSE-TOOLS ORIGINAL: Util_SafeStrdup(s) from util.h. */
#define Util_SafeStrdup(s)  strdup(s)
/* ROSE-TOOLS END */

/* ROSE-TOOLS BEGIN: ARRAYSIZE not defined without util.h/vm_basic_defs.h.
 * ROSE-TOOLS ORIGINAL: #define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0])) */
#ifndef ARRAYSIZE
#  define ARRAYSIZE(a)  (sizeof(a) / sizeof((a)[0]))
#endif
/* ROSE-TOOLS END */

#define LGPFX "RpcChannel: "

/* ROSE-TOOLS BEGIN: gUseBackdoorOnly always TRUE - no vSocket in this build.
 * ROSE-TOOLS ORIGINAL: static gboolean gUseBackdoorOnly = FALSE; */
static gboolean gUseBackdoorOnly = TRUE;
/* ROSE-TOOLS END */

#define RPCCHANNEL_VSOCKET_RETRY_MIN_DELAY    (2)
#define RPCCHANNEL_VSOCKET_RETRY_MAX_DELAY    (5 * 60)

static void RpcChannelStopNoLock(RpcChannel *chan);

/* ROSE-TOOLS BEGIN: entire NEED_RPCIN block omitted.
 * Contains: RpcChannelPing, gRpcHandlers[], RpcChannelRestart,
 * RpcChannelCheckReset, RpcChannelReset, RpcChannelXdrWrapper,
 * RpcChannel_BuildXdrCommand, RpcChannel_Dispatch,
 * RpcChannel_Setup, RpcChannelTeardown,
 * RpcChannel_RegisterCallback, RpcChannel_UnregisterCallback,
 * RpcChannelClearError, RpcChannelError.
 * Inbound dispatch is handled by rpcin.c + toolsRpc.c directly.
 * ROSE-TOOLS ORIGINAL:
 *   #if defined(NEED_RPCIN)
 *   ... (lines 91-700 of rpcChannel.c)
 *   #endif
 */
/* ROSE-TOOLS END */


/**
 * Creates a new RpcChannel without any implementation.
 *
 * This is mainly for use of code that is implementing a custom RpcChannel.
 * Such implementations should provide their own "constructor"-type function
 * which should then call this function to get an RpcChannel instance. They
 * should then fill in the function pointers that provide the implementation
 * for the channel before making the channel available to the callers.
 *
 * @return A new RpcChannel instance.
 */

RpcChannel *
RpcChannel_Create(void)
{
   /* ROSE-TOOLS BEGIN: g_new0 macro defined in shim block above.
    * ROSE-TOOLS ORIGINAL: RpcChannelInt *chan = g_new0(RpcChannelInt, 1); */
   RpcChannelInt *chan = g_new0(RpcChannelInt, 1);
   /* ROSE-TOOLS END */
   chan->impl.vsockRetryDelay = RPCCHANNEL_VSOCKET_RETRY_MIN_DELAY;
   return &chan->impl;
}


/**
 * Shuts down an RPC channel and releases any held resources.
 *
 * @param[in]  chan     The RPC channel.
 */

void
RpcChannel_Destroy(RpcChannel *chan)
{
   if (chan == NULL) {
      return;
   }

   g_mutex_lock(&chan->outLock);

   RpcChannelStopNoLock(chan);

   if (chan->funcs != NULL && chan->funcs->shutdown != NULL) {
      chan->funcs->shutdown(chan);
   }

   /* ROSE-TOOLS BEGIN: NEED_RPCIN block omitted.
    * ROSE-TOOLS ORIGINAL:
    *   #if defined(NEED_RPCIN)
    *   RpcChannelTeardown(chan);
    *   #endif
    */
   /* ROSE-TOOLS END */

   g_mutex_unlock(&chan->outLock);
   g_mutex_clear(&chan->outLock);

   g_free(chan);
}


/**
 * Sets the non-freeable result of the given RPC context to the given value.
 *
 * @param[in] data     RPC context.
 * @param[in] result   Result string.
 * @param[in] retVal   Return value of this function.
 *
 * @return @a retVal
 */

gboolean
RpcChannel_SetRetVals(RpcInData *data,
                      char const *result,
                      gboolean retVal)
{
   ASSERT(data);
   data->result = (char *)result;
   data->resultLen = strlen(data->result);
   data->freeResult = FALSE;
   return retVal;
}


/**
 * Sets the freeable result of the given RPC context to the given value.
 *
 * @param[in] data     RPC context.
 * @param[in] result   Result string.
 * @param[in] retVal   Return value of this function.
 *
 * @return @a retVal
 */

gboolean
RpcChannel_SetRetValsF(RpcInData *data,
                       char *result,
                       gboolean retVal)
{
   ASSERT(data);
   data->result = result;
   data->resultLen = strlen(data->result);
   data->freeResult = TRUE;
   return retVal;
}


/**
 * Force backdoor-only channel selection.
 */

void
RpcChannel_SetBackdoorOnly(void)
{
   gUseBackdoorOnly = TRUE;
   Debug(LGPFX "Using vsocket is disabled.\n");
}


/**
 * Create a one-off RpcChannel instance.
 * ROSE-TOOLS: always BackdoorChannel_New(), vSocket not available.
 */

static RpcChannel *
RpcChannel_NewOne(int flags)
{
   /* ROSE-TOOLS BEGIN: vSocket path omitted - backdoor only.
    * ROSE-TOOLS ORIGINAL:
    *   #if (defined(__linux__) && !defined(USERWORLD)) || defined(_WIN32)
    *   chan = gUseBackdoorOnly ? BackdoorChannel_New() : VSockChannel_New(flags);
    *   #else
    *   chan = BackdoorChannel_New();
    *   #endif
    */
   (void)flags;
   return BackdoorChannel_New();
   /* ROSE-TOOLS END */
}


/**
 * Create an RpcChannel instance.
 *
 * @return  RpcChannel
 */

RpcChannel *
RpcChannel_New(void)
{
   return RpcChannel_NewOne(0);
}


/**
 * Start an RPC channel.
 *
 * @param[in]  chan        The RPC channel instance.
 *
 * @return TRUE on success.
 */

gboolean
RpcChannel_Start(RpcChannel *chan)
{
   gboolean ok;
   const RpcChannelFuncs *funcs;

   if (chan == NULL || chan->funcs == NULL || chan->funcs->start == NULL) {
      return FALSE;
   }

   if (chan->outStarted) {
      /* ROSE-TOOLS BEGIN: NEED_RPCIN inStarted sync omitted.
       * ROSE-TOOLS ORIGINAL:
       *   #if defined(NEED_RPCIN)
       *   ASSERT(chan->in == NULL || chan->inStarted);
       *   #endif
       */
      return TRUE;
      /* ROSE-TOOLS END */
   }

   /* ROSE-TOOLS BEGIN: NEED_RPCIN RpcIn_start call omitted.
    * ROSE-TOOLS ORIGINAL:
    *   #if defined(NEED_RPCIN)
    *   if (chan->in != NULL && !chan->inStarted) {
    *      ok = RpcIn_start(chan->in, RPCIN_MAX_DELAY, RpcChannelError,
    *                       RpcChannelClearError, chan);
    *      chan->inStarted = ok;
    *   }
    *   #endif
    */
   /* ROSE-TOOLS END */

   funcs = chan->funcs;

   /* ROSE-TOOLS BEGIN: vSocket restore path omitted - backdoor only.
    * ROSE-TOOLS ORIGINAL:
    *   #if (defined(__linux__) && !defined(USERWORLD)) || defined(_WIN32)
    *   if (!gUseBackdoorOnly && chan->isMutable && ...)
    *      VSockChannel_Restore(...);
    *   #endif
    */
   /* ROSE-TOOLS END */

   ok = funcs->start(chan);

   /* ROSE-TOOLS BEGIN: vSocket fallback on start failure omitted.
    * BackdoorChannel never needs to fall back to another channel type.
    * ROSE-TOOLS ORIGINAL:
    *   if (!ok && chan->isMutable &&
    *       funcs->getType(chan) != RPCCHANNEL_TYPE_BKDOOR) {
    *      ... BackdoorChannel_Fallback(chan); ...
    *   }
    */
   /* ROSE-TOOLS END */

   return ok;
}


/**
 * Stop the RPC channel. outLock must be held by caller.
 *
 * @param[in]  chan        The RPC channel instance.
 */

static void
RpcChannelStopNoLock(RpcChannel *chan)
{
   if (chan == NULL || chan->funcs == NULL || chan->funcs->stop == NULL) {
      return;
   }

   chan->funcs->stop(chan);

   /* ROSE-TOOLS BEGIN: NEED_RPCIN RpcIn_stop call omitted.
    * ROSE-TOOLS ORIGINAL:
    *   #if defined(NEED_RPCIN)
    *   if (chan->in != NULL) {
    *      if (chan->inStarted) {
    *         RpcIn_stop(chan->in);
    *         chan->inStarted = FALSE;
    *      }
    *   } else {
    *      ASSERT(!chan->inStarted);
    *   }
    *   #endif
    */
   /* ROSE-TOOLS END */
}


/**
 * Wrapper for the stop function of an RPC channel.
 *
 * @param[in]  chan        The RPC channel instance.
 */

void
RpcChannel_Stop(RpcChannel *chan)
{
   g_mutex_lock(&chan->outLock);
   RpcChannelStopNoLock(chan);
   g_mutex_unlock(&chan->outLock);
}


/**
 * Returns the channel type.
 *
 * @param[in]  chan        The RPC channel instance.
 */

RpcChannelType
RpcChannel_GetType(RpcChannel *chan)
{
   if (chan == NULL || chan->funcs == NULL || chan->funcs->getType == NULL) {
      return RPCCHANNEL_TYPE_INACTIVE;
   }
   return chan->funcs->getType(chan);
}


/**
 * Free a result buffer returned by RpcChannel_Send*.
 *
 * @param[in] ptr   Buffer to free.
 */

void
RpcChannel_Free(void *ptr)
{
   free(ptr);
}


/**
 * Send function of an RPC channel struct.
 *
 * @param[in]  chan        The RPC channel instance.
 * @param[in]  data        Data to send.
 * @param[in]  dataLen     Number of bytes to send.
 * @param[out] result      Response (free with RpcChannel_Free).
 * @param[out] resultLen   Number of bytes in response.
 *
 * @return The status from the remote end.
 */

gboolean
RpcChannel_Send(RpcChannel *chan,
                char const *data,
                size_t dataLen,
                char **result,
                size_t *resultLen)
{
   gboolean ok;
   Bool rpcStatus;
   char *res = NULL;
   size_t resLen = 0;
   const RpcChannelFuncs *funcs;

   Debug(LGPFX "Sending: %"FMTSZ"u bytes\n", dataLen);

   ASSERT(chan && chan->funcs);

   g_mutex_lock(&chan->outLock);

   funcs = chan->funcs;
   ASSERT(funcs->send);

   if (result != NULL) {
      *result = NULL;
   }
   if (resultLen != NULL) {
      *resultLen = 0;
   }

   /* ROSE-TOOLS BEGIN: vSocket retry-on-send path omitted - backdoor only.
    * BackdoorChannel does not use vsockFailureTS / isMutable.
    * ROSE-TOOLS ORIGINAL:
    *   #if (defined(__linux__) && !defined(USERWORLD)) || defined(_WIN32)
    *   if (chan->isMutable && funcs->getType(chan) == RPCCHANNEL_TYPE_BKDOOR) {
    *      gboolean tryVSocket = ...;
    *      if (tryVSocket && funcs->stop != NULL) { ... RpcChannel_Start ... }
    *   }
    *   #endif
    */
   /* ROSE-TOOLS END */

   ok = funcs->send(chan, data, dataLen, &rpcStatus, &res, &resLen);

   /* ROSE-TOOLS BEGIN: vSocket send-failure retry omitted - backdoor only.
    * ROSE-TOOLS ORIGINAL:
    *   if (!ok && (funcs->getType(chan) != RPCCHANNEL_TYPE_BKDOOR) && ...) {
    *      ... stop, restart, retry ...
    *   }
    */
   /* ROSE-TOOLS END */

   if (ok) {
      Debug(LGPFX "Recved %"FMTSZ"u bytes\n", resLen);
   }

   if (result != NULL) {
      *result = res;
   } else {
      free(res);
   }
   if (resultLen != NULL) {
      *resultLen = resLen;
   }

   g_mutex_unlock(&chan->outLock);
   return ok && rpcStatus;
}


/**
 * Open/close RpcChannel each time for a single send. Internal helper.
 *
 * @param[in]  data        Request data.
 * @param[in]  dataLen     Data length.
 * @param[out] result      Reply (free with RpcChannel_Free).
 * @param[out] resultLen   Reply length.
 * @param[in]  priv        Privileged channel request (always FALSE here).
 *
 * @return TRUE on success.
 */

static gboolean
RpcChannelSendOneRaw(const char *data,
                     size_t dataLen,
                     char **result,
                     size_t *resultLen,
                     gboolean priv)
{
   RpcChannel *chan;
   gboolean status = FALSE;

   /* ROSE-TOOLS BEGIN: RPCCHANNEL_FLAGS_SEND_ONE / FAST_CLOSE and vSocket
    * priv path omitted. Flags only affect vSocket channel selection.
    * ROSE-TOOLS ORIGINAL:
    *   int flags = RPCCHANNEL_FLAGS_SEND_ONE;
    *   #if (defined(__linux__) && !defined(USERWORLD)) || defined(_WIN32)
    *   flags |= RPCCHANNEL_FLAGS_FAST_CLOSE;
    *   chan = priv ? VSockChannel_New(flags) : RpcChannel_NewOne(flags);
    *   #else
    *   chan = RpcChannel_NewOne(flags);
    *   #endif
    */
   (void)priv;
   chan = RpcChannel_NewOne(0);
   /* ROSE-TOOLS END */

   if (chan == NULL) {
      if (result != NULL) {
         *result = Util_SafeStrdup("RpcChannel: Unable to create "
                                   "the RpcChannel object");
         if (resultLen != NULL) {
            *resultLen = strlen(*result);
         }
      }
      goto sent;
   } else if (!RpcChannel_Start(chan)) {
      if (result != NULL) {
         *result = Util_SafeStrdup("RpcChannel: Unable to open the "
                                   "communication channel");
         if (resultLen != NULL) {
            *resultLen = strlen(*result);
         }
      }
      goto sent;
   /* ROSE-TOOLS BEGIN: privileged vSocket type-check omitted.
    * ROSE-TOOLS ORIGINAL:
    *   } else if (priv && RpcChannel_GetType(chan) != RPCCHANNEL_TYPE_PRIV_VSOCK) {
    *      *result = Util_SafeStrdup(RPCCHANNEL_SEND_PERMISSION_DENIED);
    *      goto sent;
    */
   /* ROSE-TOOLS END */
   } else if (!RpcChannel_Send(chan, data, dataLen, result, resultLen)) {
      goto sent;
   }

   status = TRUE;

sent:
   Debug(LGPFX "Request %s: reqlen=%"FMTSZ"u, replyLen=%"FMTSZ"u\n",
         status ? "OK" : "FAILED", dataLen, resultLen ? *resultLen : 0);
   if (chan) {
      RpcChannel_Stop(chan);
      RpcChannel_Destroy(chan);
   }

   return status;
}


/**
 * Open/close RpcChannel each time for sending a single RPC message.
 *
 * @param[in]  data        Request data.
 * @param[in]  dataLen     Data length.
 * @param[out] result      Reply (free with RpcChannel_Free).
 * @param[out] resultLen   Reply length.
 *
 * @return TRUE on success.
 */

gboolean
RpcChannel_SendOneRaw(const char *data,
                      size_t dataLen,
                      char **result,
                      size_t *resultLen)
{
   return RpcChannelSendOneRaw(data, dataLen, result, resultLen, FALSE);
}


/* ROSE-TOOLS BEGIN: RpcChannel_SendOneRawPriv omitted - no privileged
 * vSocket channel in this build.
 * ROSE-TOOLS ORIGINAL:
 *   #if defined(__linux__) || defined(_WIN32)
 *   gboolean RpcChannel_SendOneRawPriv(...) { ... }
 *   #endif
 */
/* ROSE-TOOLS END */


/**
 * Format and send a single RPC. Internal helper.
 */

static gboolean
RpcChannelSendOne(char **reply,
                  size_t *repLen,
                  char const *reqFmt,
                  va_list args,
                  gboolean priv)
{
   gboolean status;
   char *request;
   size_t reqLen = 0;

   request = Str_Vasprintf(&reqLen, reqFmt, args);
   if (request == NULL) {
      if (reply)  { *reply  = NULL; }
      if (repLen) { *repLen = 0;    }
      return FALSE;
   }

   status = RpcChannelSendOneRaw(request, reqLen, reply, repLen, priv);
   free(request);
   return status;
}


/**
 * Open/close RpcChannel each time for sending a formatted RPC message.
 *
 * @param[out] reply       Reply (free with RpcChannel_Free).
 * @param[out] repLen      Reply length.
 * @param[in]  reqFmt      printf-style format string.
 * @param[in]  ...         Format arguments.
 *
 * @return TRUE on success.
 */

gboolean
RpcChannel_SendOne(char **reply,
                   size_t *repLen,
                   char const *reqFmt,
                   ...)
{
   va_list args;
   gboolean status;

   va_start(args, reqFmt);
   status = RpcChannelSendOne(reply, repLen, reqFmt, args, FALSE);
   va_end(args);
   return status;
}


/* ROSE-TOOLS BEGIN: RpcChannel_SendOnePriv omitted - no privileged
 * vSocket channel in this build.
 * ROSE-TOOLS ORIGINAL:
 *   #if defined(__linux__) || defined(_WIN32)
 *   gboolean RpcChannel_SendOnePriv(...) { ... }
 *   #endif
 */
/* ROSE-TOOLS END */


/*
 * BackdoorChannel_New --
 *
 * Allocates and returns a new backdoor-backed RpcChannel.
 * This is the sole channel implementation in this build.
 * The implementation lives here rather than in a separate
 * backdoorChannel.c because that file is not in the OVT source
 * distribution; we reconstruct the minimum needed.
 *
 * The vtable has start/stop/send/getType. setup/shutdown/destroy
 * are NULL - RpcChannel_Create/Destroy handle allocation directly.
 */

/* Internal state for the backdoor channel. */
typedef struct BackdoorChannelData {
   RpcChannel chan;    /* must be first */
   RpcOut    *out;
} BackdoorChannelData;

static gboolean
BackdoorChannelStart(RpcChannel *chan)
{
   BackdoorChannelData *bd = (BackdoorChannelData *)chan;
   if (bd->out == NULL) {
      bd->out = RpcOut_Construct();
      if (bd->out == NULL) {
         return FALSE;
      }
   }
   if (RpcOut_start(bd->out)) {
      chan->outStarted = TRUE;
      return TRUE;
   }
   return FALSE;
}

static void
BackdoorChannelStop(RpcChannel *chan)
{
   BackdoorChannelData *bd = (BackdoorChannelData *)chan;
   if (bd->out != NULL && chan->outStarted) {
      RpcOut_stop(bd->out);
      chan->outStarted = FALSE;
   }
}

static gboolean
BackdoorChannelSend(RpcChannel *chan,
                    char const *data,
                    size_t dataLen,
                    Bool *rpcStatus,
                    char **result,
                    size_t *resultLen)
{
   BackdoorChannelData *bd = (BackdoorChannelData *)chan;
   const char *reply = NULL;
   size_t repLen = 0;
   gboolean ret;

   ret = (gboolean)RpcOut_send(bd->out, data, dataLen,
                               rpcStatus, &reply, &repLen);
   if (result != NULL && reply != NULL) {
      *result = (char *)malloc(repLen + 1);
      if (*result != NULL) {
         memcpy(*result, reply, repLen);
         (*result)[repLen] = '\0';
      }
   }
   if (resultLen != NULL) {
      *resultLen = repLen;
   }
   return ret;
}

static RpcChannelType
BackdoorChannelGetType(RpcChannel *chan)
{
   (void)chan;
   return RPCCHANNEL_TYPE_BKDOOR;
}

static void
BackdoorChannelShutdown(RpcChannel *chan)
{
   BackdoorChannelData *bd = (BackdoorChannelData *)chan;
   if (bd->out != NULL) {
      RpcOut_Destruct(bd->out);
      bd->out = NULL;
   }
}

RpcChannel *
BackdoorChannel_New(void)
{
   static const RpcChannelFuncs funcs = {
      BackdoorChannelStart,
      BackdoorChannelStop,
      BackdoorChannelSend,
      NULL,                    /* setup   - unused in USE_RPCI_ONLY */
      BackdoorChannelShutdown,
      BackdoorChannelGetType,
      NULL                     /* destroy - RpcChannel_Destroy handles free */
   };

   BackdoorChannelData *bd =
      (BackdoorChannelData *)calloc(1, sizeof *bd);
   if (bd == NULL) {
      return NULL;
   }
   bd->out = RpcOut_Construct();
   if (bd->out == NULL) {
      free(bd);
      return NULL;
   }
   bd->chan.funcs = &funcs;
   bd->chan.vsockRetryDelay = RPCCHANNEL_VSOCKET_RETRY_MIN_DELAY;
   g_mutex_init(&bd->chan.outLock);
   return &bd->chan;
}

