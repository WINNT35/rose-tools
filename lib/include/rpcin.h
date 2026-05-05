/*********************************************************
 * Copyright (c) 1998-2020, 2022 VMware, Inc. All rights reserved.
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

#ifndef _RPCIN_H_
#define _RPCIN_H_

/*
 * rpcin.h --
 *
 *   Public interface for the inbound (host->guest) RPC channel.
 *
 * ROSE-TOOLS NOTE: This header is written from scratch for the non-GLib,
 * non-vSocket build path (VMTOOLS_USE_GLIB and VMTOOLS_USE_VSOCKET both
 * undefined). It declares only the symbols used in that path.
 * The upstream rpcin.h would conditionally declare different signatures
 * for RpcIn_Construct and RpcIn_start depending on VMTOOLS_USE_GLIB.
 * We expose only the non-GLib variants.
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stddef.h>
#include "vm_basic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum poll delay in centiseconds passed to RpcIn_start.
 * Mirrors RPCIN_MAX_DELAY_CS from upstream tools usage.
 */
#define RPCIN_MAX_DELAY_CS   10

/*
 * Opaque RpcIn handle.
 */
typedef struct RpcIn RpcIn;

/*
 * ROSE-TOOLS NOTE: The non-GLib RpcIn_OldCallback has the old-style signature
 * below -- NOT the gboolean (*)(RpcInData *) signature from guestrpc.h.
 * The two are different types used in different contexts:
 *   - Old-style: registered via RpcIn_RegisterCallback, dispatched by rpcin.c
 *   - New-style: registered via RpcChannel_RegisterCallback, dispatched by
 *                rpcChannel.c (not yet implemented in rose-tools).
 * Keep them separate. Do not conflate.
 */
/* ROSE-TOOLS BEGIN: renamed from RpcIn_Callback to RpcIn_OldCallback to avoid
 * collision with the new-style RpcIn_Callback in guestrpc.h:
 *   guestrpc.h: typedef gboolean (*RpcIn_Callback)(RpcInData *data)
 *   rpcin.c:    typedef unsigned int (*RpcIn_Callback)(char const **result,...)
 * These are genuinely different types for different dispatch paths.
 * ROSE-TOOLS ORIGINAL: RpcIn_Callback */
typedef unsigned int (*RpcIn_OldCallback)(char const **result,
                                       size_t      *resultLen,
                                       const char  *name,
                                       const char  *args,
                                       size_t       argsSize,
                                       void        *clientData);
/* ROSE-TOOLS END */

/* Called by rpcin.c when a channel error occurs. */
typedef void (RpcIn_ErrorFunc)(void *data, char const *errmsg);

/* Called by rpcin.c when the error condition clears. */
typedef void (RpcIn_ClearErrorFunc)(void *data);

/*
 * DblLnkLst_Links -- opaque forward declaration.
 * RpcIn_Construct takes a pointer to the timer event queue in upstream.
 * In rose-tools this argument is ignored; pass NULL or &gRoseTimerQueueStorage.
 */
/* ROSE-TOOLS BEGIN: guard matches definition in rpcin.c shim block. */
#ifndef DBLLNKLST_LINKS_DEFINED
#define DBLLNKLST_LINKS_DEFINED
typedef struct { int _unused; } DblLnkLst_Links;
#endif
/* ROSE-TOOLS END */

/* Defined in rpcin.c -- pass to RpcIn_Construct if not using NULL. */
extern DblLnkLst_Links gRoseTimerQueueStorage;


/*
 * Lifecycle
 */

RpcIn *RpcIn_Construct(DblLnkLst_Links *eventQueue);
void   RpcIn_Destruct(RpcIn *in);

Bool   RpcIn_start(RpcIn               *in,
                   unsigned int         delay,
                   RpcIn_OldCallback       resetCallback,
                   void                *resetClientData,
                   RpcIn_ErrorFunc     *errorFunc,
                   RpcIn_ClearErrorFunc *clearErrorFunc,
                   void                *errorData);

void   RpcIn_stop(RpcIn *in);

/*
 * ROSE-TOOLS: RpcIn_Poll drives one TCLO receive iteration.
 * Call from your main loop every tick instead of EventManager.
 * Returns TRUE if healthy, FALSE if the channel has stopped.
 */
Bool   RpcIn_Poll(RpcIn *in);


/*
 * Callback registration
 */

void   RpcIn_RegisterCallback(RpcIn          *in,
                               const char     *name,
                               RpcIn_OldCallback  cb,
                               void           *clientData);

void   RpcIn_UnregisterCallback(RpcIn      *in,
                                 const char *name);


/*
 * Utility: set the return values of a TCLO callback.
 */
unsigned int RpcIn_SetRetVals(char const **result,
                               size_t      *resultLen,
                               const char  *resultVal,
                               Bool         retVal);

#ifdef __cplusplus
}
#endif

#endif /* _RPCIN_H_ */

