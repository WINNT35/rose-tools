/*********************************************************
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
 * @file timeSyncWin.c
 *
 * Windows platform implementations of the TimeSync platform abstraction
 * layer declared in timeSync.h.
 *
 * Replaces timeSyncPosix.c, slewAdjtime.c, slewLinux.c, pllLinux.c,
 * and pllNone.c from upstream. Those files are POSIX/Linux only and
 * are not used on Windows.
 *
 * v0.2 scope:
 *   - TimeSync_GetCurrentTime  -- GetSystemTimeAsFileTime -> microseconds
 *   - TimeSync_AddToCurrentTime -- SetSystemTime (step only)
 *   - TimeSync_Slew            -- stub, returns FALSE (TODO v0.3)
 *   - TimeSync_DisableTimeSlew -- stub, returns TRUE
 *   - TimeSync_PLLSupported    -- returns FALSE (TODO v0.3)
 *   - TimeSync_PLLUpdate       -- stub, returns FALSE
 *   - TimeSync_PLLSetFrequency -- stub, returns FALSE
 *   - TimeSync_IsGuestSyncServiceRunning -- returns FALSE (no w32time check)
 *   - TimeSync_DoGuestResync   -- stub, returns FALSE
 *
 * TODO v0.3:
 *   - Slew via SetSystemTimeAdjustment (available on NT 3.1+)
 *   - PLL support
 *   - w32time resync via W32TM /resync
 *
 * Target: Windows NT 3.1+ 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <windows.h>
#include "vm_basic_types.h"
#include "timeSync.h"

/* Unix epoch to Windows FILETIME epoch delta in microseconds
 * Jan 1, 1601 to Jan 1, 1970 = 134,774 days */
#define EPOCH_DELTA_US  (134774ULL * 24ULL * 3600ULL * 1000000ULL)


/**
 * Get the current guest OS time in microseconds since Unix epoch.
 *
 * Mirrors TimeSync_GetCurrentTime in timeSyncPosix.c.
 * Uses GetSystemTimeAsFileTime which is available on NT 3.1+.
 *
 * @param[out] now  Current time in microseconds since Unix epoch.
 * @return TRUE on success.
 */
Bool
TimeSync_GetCurrentTime(int64 *now)
{
   FILETIME       ft;
   unsigned long long t;

   GetSystemTimeAsFileTime(&ft);

   t  = (unsigned long long)ft.dwHighDateTime << 32;
   t |= (unsigned long long)ft.dwLowDateTime;

   /* Convert from 100ns ticks since 1601 to microseconds since 1970 */
   t /= 10;            /* 100ns -> microseconds */
   t -= EPOCH_DELTA_US;

   *now = (int64)t;
   return TRUE;
}


/**
 * Add delta microseconds to the current system time by stepping.
 *
 * Mirrors TimeSync_AddToCurrentTime in timeSyncPosix.c.
 * Uses SetSystemTime which requires SE_SYSTEMTIME_NAME privilege
 * (acquired in TimeSync_Init).
 *
 * @param[in] delta  Microseconds to add (may be negative).
 * @return TRUE on success.
 */
Bool
TimeSync_AddToCurrentTime(int64 delta)
{
   int64          now;
   int64          newTime;
   unsigned long long t;
   FILETIME       ft;
   SYSTEMTIME     st;

   if (!TimeSync_GetCurrentTime(&now)) {
      return FALSE;
   }

   newTime = now + delta;
   if (newTime <= 0) {
      fprintf(stderr, "timeSync: new time would be <= 0, ignoring.\n");
      return FALSE;
   }

   /* Convert microseconds since Unix epoch back to FILETIME */
   t  = (unsigned long long)newTime;
   t += EPOCH_DELTA_US;
   t *= 10;   /* microseconds -> 100ns ticks */

   ft.dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
   ft.dwHighDateTime = (DWORD)(t >> 32);

   if (!FileTimeToSystemTime(&ft, &st)) {
      fprintf(stderr, "timeSync: FileTimeToSystemTime failed (err=%lu)\n",
              GetLastError());
      return FALSE;
   }

   if (!SetSystemTime(&st)) {
      fprintf(stderr, "timeSync: SetSystemTime failed (err=%lu)\n",
              GetLastError());
      return FALSE;
   }

   return TRUE;
}


/**
 * Slew the system clock by delta microseconds over timeSyncPeriod microseconds.
 *
 * TODO v0.3: implement via SetSystemTimeAdjustment.
 * For v0.2 we return FALSE so the caller falls back to stepping.
 *
 * @param[in]  delta           Microseconds of error to correct.
 * @param[in]  timeSyncPeriod  Period over which to correct (microseconds).
 * @param[out] remaining       Remaining error after correction.
 * @return FALSE (not implemented).
 */
Bool
TimeSync_Slew(int64 delta, int64 timeSyncPeriod, int64 *remaining)
{
   (void)delta;
   (void)timeSyncPeriod;
   if (remaining != NULL) {
      *remaining = delta;
   }
   return FALSE;
}


/**
 * Disable any active time slew (reset to nominal rate).
 *
 * TODO v0.3: call SetSystemTimeAdjustment with bTimeAdjustmentDisabled=TRUE.
 *
 * @return TRUE (no-op for v0.2).
 */
Bool
TimeSync_DisableTimeSlew(void)
{
   return TRUE;
}


/**
 * PLL functions - not implemented for v0.2.
 * TODO v0.3.
 */
Bool TimeSync_PLLSupported(void)    { return FALSE; }
Bool TimeSync_PLLUpdate(int64 o)    { (void)o; return FALSE; }
Bool TimeSync_PLLSetFrequency(int64 p) { (void)p; return FALSE; }


/**
 * Check if a guest time sync service (e.g. w32time) is running.
 *
 * TODO v0.3: query SCM for W32Time service status.
 *
 * @return FALSE (not implemented).
 */
Bool
TimeSync_IsGuestSyncServiceRunning(void)
{
   return FALSE;
}


/**
 * Request guest time sync service to resync.
 *
 * TODO v0.3: invoke W32TM /resync via CreateProcess.
 *
 * @return FALSE (not implemented).
 */
Bool
TimeSync_DoGuestResync(void *_ctx)
{
   (void)_ctx;
   return FALSE;
}
