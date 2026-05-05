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
 * rose-tools
 * lib/stubs/stub-rpc.c

/*
 * stub-rpc.c --
 *
 * Linker stubs for RPC channel functions used by plugin DLLs.
 *
 * Plugin DLLs call RpcChannel_Send at runtime through vmrosd.exe.
 * These stubs satisfy the linker at build time only -- they are never
 * called at runtime because the real symbols are resolved from the
 * host process when the DLL is loaded via LoadLibrary.
 *
 * ROSE-TOOLS ORIGINAL: these stubs have no upstream equivalent.
 * open-vm-tools plugins link against a shared libvmtools which provides
 * these symbols. We have no shared lib, so we stub them for DLL builds.
 */

#include <stddef.h>

typedef struct RpcChannel RpcChannel;

int
RpcChannel_Send(RpcChannel *chan,
                char const *data,
                size_t dataLen,
                char **result,
                size_t *resultLen)
{
   /* Never called - resolved from vmrosd.exe at runtime */
   (void)chan;
   (void)data;
   (void)dataLen;
   if (result)    *result    = NULL;
   if (resultLen) *resultLen = 0;
   return 0;
}
