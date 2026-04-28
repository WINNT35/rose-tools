/*
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
 * rose-tools
 * lib/rpcChannel/rpcChannel_rose.c
 *
 * TODO: Upgrade in v0.2. Using the open-vm-tools rpcChannel.c as a
 * reference, implement full channel functionality in this file:
 * vSockets, inbound RPC (NEED_RPCIN), channel reset callbacks, and
 * XDR. This file stays and grows in place. Interface is intentionally
 * identical to upstream so callers need no changes when the
 * implementation is upgraded.
 *
 * Implements RpcChannel over RpcOut (backdoor only).
 * vSockets, XDR, inbound RPC (NEED_RPCIN), and channel reset callbacks
 * are not implemented - these are v0.2+ work.
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdlib.h>
#include <string.h>

#include "vmware/tools/guestrpc.h"
#include "rpcout.h"
#include "vm_basic_types.h"

/*
 * Internal channel state.
 * Wraps an RpcOut handle for the backdoor path.
 */
struct _RpcChannel {
   RpcOut *out;
   int     started;
};

/* Backdoor-only flag - matches upstream RpcChannel_SetBackdoorOnly behavior */
static int gUseBackdoorOnly = 1;   /* always TRUE for v0.1 */


/*
 * RpcChannel_New --
 *
 * Allocates a new RpcChannel. Caller must call RpcChannel_Start before
 * sending. Mirrors upstream RpcChannel_New which selects backdoor vs vSocket.
 * v0.1: always returns a backdoor channel.
 */
RpcChannel *
RpcChannel_New(void)
{
   return BackdoorChannel_New();
}


/*
 * BackdoorChannel_New --
 *
 * Allocates a new backdoor-backed RpcChannel.
 */
RpcChannel *
BackdoorChannel_New(void)
{
   RpcChannel *chan = (RpcChannel *)calloc(1, sizeof *chan);
   if (chan == NULL) {
      return NULL;
   }
   chan->out     = RpcOut_Construct();
   chan->started = 0;
   if (chan->out == NULL) {
      free(chan);
      return NULL;
   }
   return chan;
}


/*
 * RpcChannel_Create --
 *
 * Mirrors upstream RpcChannel_Create.
 * v0.1: equivalent to RpcChannel_New.
 */
RpcChannel *
RpcChannel_Create(void)
{
   return RpcChannel_New();
}


/*
 * RpcChannel_Destroy --
 *
 * Stops and frees the channel.
 */
void
RpcChannel_Destroy(RpcChannel *chan)
{
   if (chan == NULL) {
      return;
   }
   if (chan->started) {
      RpcChannel_Stop(chan);
   }
   RpcOut_Destruct(chan->out);
   free(chan);
}


/*
 * RpcChannel_Start --
 *
 * Opens the backdoor channel. Must be called before RpcChannel_Send.
 * Returns TRUE on success.
 */
gboolean
RpcChannel_Start(RpcChannel *chan)
{
   if (chan == NULL || chan->out == NULL) {
      return FALSE;
   }
   if (chan->started) {
      return TRUE;
   }
   chan->started = RpcOut_start(chan->out);
   return chan->started ? TRUE : FALSE;
}


/*
 * RpcChannel_Stop --
 *
 * Closes the backdoor channel.
 */
void
RpcChannel_Stop(RpcChannel *chan)
{
   if (chan == NULL || !chan->started) {
      return;
   }
   RpcOut_stop(chan->out);
   chan->started = 0;
}


/*
 * RpcChannel_GetType --
 *
 * Returns the channel type. v0.1: always backdoor.
 */
RpcChannelType
RpcChannel_GetType(RpcChannel *chan)
{
   (void)chan;
   return RPCCHANNEL_TYPE_BKDOOR;
}


/*
 * RpcChannel_Send --
 *
 * Sends a command over the channel and receives a reply.
 * Returns TRUE if the host acknowledged successfully.
 */
gboolean
RpcChannel_Send(RpcChannel *chan,
                char const *data,
                size_t dataLen,
                char **result,
                size_t *resultLen)
{
   Bool       rpcStatus = FALSE;
   const char *reply    = NULL;
   size_t     repLen    = 0;
   gboolean   ret;

   if (chan == NULL || !chan->started || data == NULL) {
      return FALSE;
   }

   ret = (gboolean)RpcOut_send(chan->out, data, dataLen,
                               &rpcStatus, &reply, &repLen);

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

   return ret && rpcStatus;
}


/*
 * RpcChannel_Free --
 *
 * Frees a buffer returned by RpcChannel_Send.
 */
void
RpcChannel_Free(void *ptr)
{
   free(ptr);
}


/*
 * RpcChannel_SendOneRaw --
 *
 * One-shot send: open, send, close. Mirrors upstream behavior.
 */
gboolean
RpcChannel_SendOneRaw(const char *data,
                      size_t dataLen,
                      char **result,
                      size_t *resultLen)
{
   return (gboolean)RpcOut_SendOneRaw((void *)data, dataLen,
                                      result, resultLen);
}


/*
 * RpcChannel_SetRetVals --
 *
 * Helper to set the return values in an RpcInData struct.
 * Mirrors upstream RPCIN_SETRETVALS.
 * v0.1: stub - inbound RPC not yet implemented.
 */
gboolean
RpcChannel_SetRetVals(RpcInData *data,
                      char const *result,
                      gboolean retVal)
{
   if (data != NULL) {
      data->result    = (char *)result;
      data->resultLen = result ? strlen(result) : 0;
      data->freeResult = FALSE;
   }
   return retVal;
}


/*
 * RpcChannel_SetRetValsF --
 *
 * Same as RpcChannel_SetRetVals but marks result for freeing.
 * v0.1: stub - inbound RPC not yet implemented.
 */
gboolean
RpcChannel_SetRetValsF(RpcInData *data,
                       char *result,
                       gboolean retVal)
{
   if (data != NULL) {
      data->result     = result;
      data->resultLen  = result ? strlen(result) : 0;
      data->freeResult = TRUE;
   }
   return retVal;
}


/*
 * RpcChannel_SetBackdoorOnly --
 *
 * Forces backdoor channel selection. v0.1: always backdoor anyway.
 */
void
RpcChannel_SetBackdoorOnly(void)
{
   gUseBackdoorOnly = 1;
}
