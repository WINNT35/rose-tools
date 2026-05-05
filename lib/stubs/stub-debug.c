/*********************************************************
 * Copyright (C) 2008 VMware, Inc. All rights reserved.
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

/*
 * stub-debug.c --
 *
 *   Stub for Debug().
 *
 */

#include <stdio.h>
#include "str.h"


void
Debug(const char *fmt, ...)
{
#ifdef VMX86_DEBUG
   char *str;
   va_list args;

   va_start(args, fmt);
   str = Str_Vasprintf(NULL, fmt, args);
   va_end(args);

   if (str != NULL) {
      fputs(str, stderr);
   }
#endif
}

/* ROSE-TOOLS BEGIN: stubs for symbols required by rpcin.c and vm_assert.h.
 * Panic is declared NORETURN in vm_assert.h - we abort() to satisfy that.
 * Log and Warning mirror Debug() - silent in non-debug builds.
 * Str_Snprintf is declared in str.h but not implemented in str_rose.c.
 * ROSE-TOOLS ORIGINAL: these were provided by panic.c, log.c, str.c in
 * open-vm-tools - none of which are in our tree. */

#include <stdarg.h>
#include <stdlib.h>
#include "vm_assert.h"

void NORETURN
Panic(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   vfprintf(stderr, fmt, args);
   va_end(args);
   fflush(stderr);
   abort();
}

void
Log(const char *fmt, ...)
{
#ifdef VMX86_DEBUG
   va_list args;
   va_start(args, fmt);
   vfprintf(stderr, fmt, args);
   va_end(args);
#else
   (void)fmt;
#endif
}

void
Warning(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   vfprintf(stderr, fmt, args);
   va_end(args);
}

int
Str_Snprintf(char *buf, size_t size, const char *fmt, ...)
{
   int ret;
   va_list args;
   va_start(args, fmt);
   ret = _vsnprintf(buf, size, fmt, args);
   va_end(args);
   if (size > 0) {
      buf[size - 1] = '\0';
   }
   return ret;
}
/* ROSE-TOOLS END */
