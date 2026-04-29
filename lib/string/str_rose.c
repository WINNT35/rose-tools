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
 * str_rose.c - rose-tools original implementation
 *
 * Provides Str_Vasprintf and Str_Asprintf using standard vsnprintf.
 * str.c from open-vm-tools is excluded from this build due to a
 * dependency on bsd_output.h which is a generated file not present
 * in the source tree. See https://github.com/vmware/open-vm-tools/issues/148
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */
 
 /* TODO: Upgrade in v0.2. Implement full functionality from 
  * open-vmtools str.c */
/*
yOOO WHATS UP ITS YOUR BOY SPACE OVER HERE WITH A NEW THINGABOBJI
*

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "str.h"

/*
 * Str_Vasprintf --
 *
 *   Allocates a buffer and formats a string into it, va_list variant.
 *   Caller is responsible for freeing the returned buffer.
 *
 *   If length is non-NULL, it receives the length of the formatted string
 *   excluding the NUL terminator.
 *
 *   Returns NULL on allocation failure.
 */

char *
Str_Vasprintf(size_t *length, const char *fmt, va_list args)
{
    char *buf;
    int needed;
    va_list args2;

    va_copy(args2, args);
    needed = _vsnprintf(NULL, 0, fmt, args2);
    va_end(args2);

    if (needed < 0) {
        return NULL;
    }

    buf = (char *)malloc((size_t)needed + 1);
    if (buf == NULL) {
        return NULL;
    }

    _vsnprintf(buf, (size_t)needed + 1, fmt, args);
    buf[needed] = '\0';

    if (length != NULL) {
        *length = (size_t)needed;
    }

    return buf;
}


/*
 * Str_Asprintf --
 *
 *   Allocates a buffer and formats a string into it, varargs variant.
 *   Caller is responsible for freeing the returned buffer.
 *
 *   Returns NULL on allocation failure.
 */

char *
Str_Asprintf(size_t *length, const char *fmt, ...)
{
    char *buf;
    va_list args;

    va_start(args, fmt);
    buf = Str_Vasprintf(length, fmt, args);
    va_end(args);

    return buf;
}
