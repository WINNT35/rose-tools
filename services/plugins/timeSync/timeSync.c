/*********************************************************
 * Copyright (c) 2008-2025 Broadcom. All Rights Reserved.
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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
 * @file timeSync.c
 *
 * Plugin to handle time synchronization between the guest and host.
 *
 * There are two types of corrections this plugin makes: one time and periodic.
 *
 * Periodic time synchronization is done when tools.timeSync is enabled
 * (this corresponds with the Synchronize Host and Guest Time checkbox in
 * the toolbox).  When it is active time is corrected once per period
 * (typically every 60 seconds).
 *
 * One time corrections are done: at tools startup, resuming from suspend,
 * after disk shrink and other times when the guest has not been running
 * for a while.
 *
 * There are two basic methods for correcting the time: stepping and slewing.
 * For v0.2 only stepping is implemented. Slewing is TODO v0.3.
 *
 * ROSE-TOOLS BEGIN: Divergences from upstream
 *
 * Removed (not applicable to Windows or v0.2 scope):
 *   - GLib (gboolean, g_malloc, g_debug, g_warning, GSource, timers)
 *     replaced with plain C types and fprintf.
 *   - conf.h / VMTools_ConfigGetBoolean replaced with RoseConfig_GetBool.
 *   - msg.h / Msg_ErrString replaced with strerror(errno) / GetLastError().
 *   - strutil.h / StrUtil_StrToInt replaced with atoi/strtoul.
 *   - system.h dependency removed.
 *   - embed_version.h / vmtoolsd_version.h removed (no GLib version embed).
 *   - TimeSyncStartLoop / TimeSyncStopLoop (GLib timer loop) replaced
 *     with rose-tools tick mechanism.
 *   - TimeSyncSlewTime and PLL code retained as stubs (TODO v0.3).
 *   - TimeSyncSetOption: synctime, synctime.period handled; others stubbed.
 *   - TimeSyncGuestResyncTimeoutHandler removed (GLib timer callback).
 *   - timeInfo.c integration removed (Linux-only, TODO v0.3).
 *   - ToolsOnLoad replaced with RoseOnLoad using rose-tools plugin API.
 *   - TOOLS_MODULE_EXPORT replaced with ROSE_MODULE_EXPORT.
 *   - RpcChannelCallback / TIMESYNC_SYNCHRONIZE TCLO handler kept.
 *   - ToolsPluginSignalCb replaced with RoseRegisterSignal calls in init.
 *
 * Platform layer (TimeSync_GetCurrentTime, TimeSync_AddToCurrentTime,
 * TimeSync_Slew, TimeSync_PLLSupported etc.) implemented in timeSyncWin.c.
 *
 * ROSE-TOOLS END
 *
 * Target: Windows NT 3.1+ 32-bit, C89, MinGW
 */

/* ROSE-TOOLS BEGIN: replaced upstream includes */
/*
 * Original upstream includes:
 * #include "timeSync.h"
 * #include "backdoor.h"
 * #include "backdoor_def.h"
 * #include "conf.h"
 * #include "msg.h"
 * #include "strutil.h"
 * #include "system.h"
 * #include "vmware/guestrpc/timesync.h"
 * #include "vmware/tools/plugin.h"
 * #include "vmware/tools/utils.h"
 * #include "timeInfo.h"
 * #include "vm_version.h"
 * #include "embed_version.h"
 * #include "vmtoolsd_version.h"
 * VM_EMBED_VERSION(VMTOOLSD_VERSION_STRING);
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "backdoor.h"
#include "backdoor_def.h"
#include "timeSync.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"
#include "roseConfig.h"
/* ROSE-TOOLS END */

/* Sync the time once a minute. */
#define TIMESYNC_TIME 60
/* Correct PERCENT_CORRECTION percent of the error each period. */
#define TIMESYNC_PERCENT_CORRECTION 50

/* When measuring the difference between time on the host and time in the
 * guest we try up to TIMESYNC_MAX_SAMPLES times to read a sample
 * where the two host reads are within TIMESYNC_GOOD_SAMPLE_THRESHOLD
 * microseconds. */
#define TIMESYNC_MAX_SAMPLES 4
#define TIMESYNC_GOOD_SAMPLE_THRESHOLD 2000

/* Once the error drops below TIMESYNC_PLL_ACTIVATE, activate the PLL.
 * 500ppm error acumulated over a 60 second interval can produce 30ms of
 * error. */
#define TIMESYNC_PLL_ACTIVATE (30 * 1000) /* 30ms. */
/* If the error goes above TIMESYNC_PLL_UNSYNC, deactivate the PLL. */
#define TIMESYNC_PLL_UNSYNC (2 * TIMESYNC_PLL_ACTIVATE)

/* ROSE-TOOLS BEGIN: config key names (mirrors CONFNAME_TIMESYNC_* in conf.h) */
#define ROSE_TIMESYNC_SECTION          "timesync"
#define ROSE_TIMESYNC_DISABLE_ALL      "disable"
#define ROSE_TIMESYNC_DISABLE_PERIODIC "disablePeriodic"
/* ROSE-TOOLS END */

/* ROSE-TOOLS BEGIN: gboolean -> int, TRUE/FALSE -> 1/0 throughout */
/* ROSE-TOOLS END */

typedef enum TimeSyncState {
   TIMESYNC_INITIALIZING,
   TIMESYNC_STOPPED,
   TIMESYNC_RUNNING,
} TimeSyncState;

typedef enum TimeSyncSlewState {
   TimeSyncUncalibrated,
   TimeSyncCalibrating,
   TimeSyncPLL,
} TimeSyncSlewState;

typedef enum TimeSyncType {
   TIMESYNC_STEP,
   TIMESYNC_PERIODIC,
   TIMESYNC_STEP_NORESYNC,
} TimeSyncType;

/* ROSE-TOOLS BEGIN: replaced GLib fields with plain equivalents.
 * Removed: GSource *timer, GSource *guestResyncTimer (GLib timers).
 * Removed: ToolsAppCtx *ctx (replaced with RoseAppCtx *ctx).
 */
typedef struct TimeSyncData {
   int                slewActive;
   int                slewCorrection;
   unsigned int       slewPercentCorrection;
   unsigned int       timeSyncPeriod;         /* In seconds. */
   TimeSyncState      state;
   TimeSyncSlewState  slewState;
   int                guestResync;
   unsigned int       guestResyncTimeout;
   RoseAppCtx        *ctx;
} TimeSyncData;
/* ROSE-TOOLS END */

/* ROSE-TOOLS BEGIN: gTimeSyncToolsStartupAllowBackward kept but type changed */
/* Original: gboolean gTimeSyncToolsStartupAllowBackward = FALSE; */
static int gTimeSyncToolsStartupAllowBackward = 0;

/* ROSE-TOOLS BEGIN: static plugin data pointer - single instance plugin */
static TimeSyncData *g_timeSyncData = NULL;
/* ROSE-TOOLS END */
/* ROSE-TOOLS END */

static void TimeSyncSetSlewState(TimeSyncData *data, int active);
static void TimeSyncResetSlew(TimeSyncData *data);
static int  TimeSyncDoSync(int slewCorrection, TimeSyncType syncType,
                           int allowBackwardSync, void *_data);


/**
 * Read the time reported by the Host OS.
 *
 * @param[out]  host                Time on the Host.
 * @param[out]  apparentError       Apparent time error = apparent - real.
 * @param[out]  apparentErrorValid  Did the platform inform us of apparentError.
 * @param[out]  maxTimeError        Maximum amount of error than can go
 *                                  uncorrected.
 *
 * @return TRUE on success.
 */

/* ROSE-TOOLS BEGIN: gboolean -> int, Bool -> int */
static int
TimeSyncReadHost(int64 *host, int64 *apparentError, int *apparentErrorValid,
                 int64 *maxTimeError)
/* ROSE-TOOLS END */
{
   Backdoor_proto bp;
   int64 maxTimeLag;
   int64 interruptLag;
   int64 hostSecs;
   int64 hostUsecs;
   /* ROSE-TOOLS BEGIN: Bool -> int */
   int timeLagCall;
   /* ROSE-TOOLS END */

   /*
    * We need 3 things from the host, and there exist 3 different versions of
    * the calls (described further below):
    * 1) host time
    * 2) maximum time lag allowed (config option), which is a
    *    threshold that keeps the tools from being over eager about
    *    resetting the time when it is only a little bit off.
    * 3) interrupt lag (the amount that apparent time lags real time)
    *
    * First 2 versions of the call add interrupt lag to the maximum allowed
    * time lag, where as in the last call it is returned separately.
    *
    * Three versions of the call:
    *
    * - BDOOR_CMD_GETTIME: suffers from a 136-year overflow problem that
    *   cannot be corrected without breaking backwards compatibility with
    *   older Tools. So, we have the newer BDOOR_CMD_GETTIMEFULL, which is
    *   overflow safe.
    *
    * - BDOOR_CMD_GETTIMEFULL: overcomes the problem above.
    *
    * - BDOOR_CMD_GETTIMEFULL_WITH_LAG: Both BDOOR_CMD_GETTIMEFULL and
    *   BDOOR_CMD_GETTIME returns max lag limit as interrupt lag + the maximum
    *   allowed time lag. BDOOR_CMD_GETTIMEFULL_WITH_LAG separates these two
    *   values. This is helpful when synchronizing time backwards by slewing
    *   the clock.
    *
    * We use BDOOR_CMD_GETTIMEFULL_WITH_LAG first and fall back to
    * BDOOR_CMD_GETTIMEFULL or BDOOR_CMD_GETTIME.
    *
    * Note that BDOOR_CMD_GETTIMEFULL and BDOOR_CMD_GETTIMEFULL_WITH_LAG will
    * not touch EAX when it succeeds. So we check for errors by comparing EAX to
    * BDOOR_MAGIC, which was set by the call to Backdoor() prior to touching the
    * backdoor port.
    */
   bp.in.cx.halfs.low = BDOOR_CMD_GETTIMEFULL_WITH_LAG;
   Backdoor(&bp);
   if (bp.out.ax.word == BDOOR_MAGIC) {
      hostSecs = ((uint64)bp.out.si.word << 32) | bp.out.dx.word;
      interruptLag = bp.out.di.word;
      timeLagCall = 1;
   } else {
      interruptLag = 0;
      timeLagCall = 0;
      bp.in.cx.halfs.low = BDOOR_CMD_GETTIMEFULL;
      Backdoor(&bp);
      if (bp.out.ax.word == BDOOR_MAGIC) {
         hostSecs = ((uint64)bp.out.si.word << 32) | bp.out.dx.word;
      } else {
         bp.in.cx.halfs.low = BDOOR_CMD_GETTIME;
         Backdoor(&bp);
         /* ROSE-TOOLS BEGIN: MAX_UINT32 not defined, use 0xFFFFFFFFUL */
         /* Original: if (bp.out.ax.word == MAX_UINT32) { */
         if (bp.out.ax.word == 0xFFFFFFFFUL) {
         /* ROSE-TOOLS END */
            hostSecs = -1;
         } else {
            hostSecs = bp.out.ax.word;
         }
      }
   }
   hostUsecs = bp.out.bx.word;
   maxTimeLag = bp.out.cx.word;

   *host = hostSecs * US_PER_SEC + hostUsecs;
   *apparentError = -interruptLag;
   *apparentErrorValid = timeLagCall;
   *maxTimeError = maxTimeLag;

   if (hostSecs <= 0) {
      fprintf(stderr, "timeSync: invalid host time: %lld secs, %lld usecs.\n",
              (long long)hostSecs, (long long)hostUsecs);
      return 0;
   }

   return 1;
}


/**
 * Read the Guest OS time and the Host OS time.
 *
 * @param[out]  host                Time on the Host.
 * @param[out]  guest               Time in the Guest.
 * @param[out]  apparentError       Apparent time error = apparent - real.
 * @param[out]  apparentErrorValid  Did the platform inform us of apparentError.
 * @param[out]  maxTimeError        Maximum amount of error than can go
 *                                  uncorrected.
 *
 * @return TRUE on success.
 */

/* ROSE-TOOLS BEGIN: gboolean -> int */
static int
TimeSyncReadHostAndGuest(int64 *host, int64 *guest,
                         int64 *apparentError, int *apparentErrorValid,
                         int64 *maxTimeError)
/* ROSE-TOOLS END */
{
   int64 host1, host2, hostDiff;
   int64 tmpGuest, tmpApparentError, tmpMaxTimeError;
   /* ROSE-TOOLS BEGIN: Bool -> int */
   int tmpApparentErrorValid;
   /* ROSE-TOOLS END */
   /* ROSE-TOOLS BEGIN: MAX_INT64 not defined, use initial 0 + unconditional first assignment */
   int64 bestHostDiff = 0x7FFFFFFFFFFFFFFFLL;
   /* ROSE-TOOLS END */
   int iter = 0;

   *apparentErrorValid = 0;
   *host = *guest = *apparentError = *maxTimeError = 0;

   if (!TimeSyncReadHost(&host2, &tmpApparentError,
                         &tmpApparentErrorValid, &tmpMaxTimeError)) {
      return 0;
   }

   do {
      iter++;
      host1 = host2;

      if (!TimeSync_GetCurrentTime(&tmpGuest)) {
         /* ROSE-TOOLS BEGIN: g_warning -> fprintf, Msg_ErrString -> strerror */
         /* Original: g_warning("Unable to retrieve the guest OS time: %s.\n\n", Msg_ErrString()); */
         fprintf(stderr, "timeSync: unable to get guest time.\n");
         /* ROSE-TOOLS END */
         return 0;
      }

      if (!TimeSyncReadHost(&host2, &tmpApparentError,
                            &tmpApparentErrorValid, &tmpMaxTimeError)) {
         return 0;
      }

      if (host1 < host2) {
         hostDiff = host2 - host1;
      } else {
         hostDiff = 0;
      }

      if (hostDiff <= bestHostDiff) {
         bestHostDiff = hostDiff;
         *host = host1 + hostDiff / 2;
         *guest = tmpGuest;
         *apparentError = tmpApparentError;
         *apparentErrorValid = tmpApparentErrorValid;
         *maxTimeError = tmpMaxTimeError;
      }
   } while (iter < TIMESYNC_MAX_SAMPLES &&
            bestHostDiff > TIMESYNC_GOOD_SAMPLE_THRESHOLD);

   return 1;
}


/**
 * Set the guest OS time to the host OS time by stepping the time.
 *
 * @param[in]  data              Structure tracking time sync state.
 * @param[in]  adjustment        Amount to correct the guest time.
 */

/* ROSE-TOOLS BEGIN: gboolean -> int */
int
TimeSyncStepTime(TimeSyncData *data, int64 adjustment)
/* ROSE-TOOLS END */
{
   Backdoor_proto bp;
   int64 before;
   int64 after;

   TimeSync_GetCurrentTime(&before);

   /* Stepping invalidates the current slew, reset to nominal. */
   TimeSyncSetSlewState(data, 0);

   if (!TimeSync_AddToCurrentTime(adjustment)) {
      return 0;
   }

   /*
    * Tell timetracker to stop trying to catch up, since we have corrected
    * both the guest OS error and the apparent time error.
    */
   bp.in.cx.halfs.low = BDOOR_CMD_STOPCATCHUP;
   Backdoor(&bp);

   TimeSync_GetCurrentTime(&after);

   /* ROSE-TOOLS BEGIN: g_debug -> fprintf, FMT64 -> lld */
   /* Original: g_debug("Time changed by %"FMT64"dus from %"FMT64"d.%06"FMT64"d -> %"FMT64"d.%06"FMT64"d\n", ...); */
   fprintf(stdout, "timeSync: time stepped by %lldus from %lld.%06lld -> %lld.%06lld\n",
           (long long)adjustment,
           (long long)(before / US_PER_SEC), (long long)(before % US_PER_SEC),
           (long long)(after  / US_PER_SEC), (long long)(after  % US_PER_SEC));
   /* ROSE-TOOLS END */

   return 1;
}


/**
 * Slew the guest OS time advancement to correct the time.
 * v0.2: stub only, returns FALSE so caller falls back to stepping.
 * TODO v0.3: implement via SetSystemTimeAdjustment + PLL.
 */

/* ROSE-TOOLS BEGIN: full slew/PLL implementation removed for v0.2.
 * Original TimeSyncSlewTime body preserved below as comment for v0.3 reference.
 *
 * static gboolean
 * TimeSyncSlewTime(TimeSyncData *data, int64 adjustment)
 * {
 *    static int64 calibrationStart;
 *    static int64 calibrationAdjustment;
 *    int64 now;
 *    int64 remaining = 0;
 *    int64 timeSyncPeriodUS = (int64)data->timeSyncPeriod * US_PER_SEC;
 *    int64 slewDiff = (adjustment * data->slewPercentCorrection) / 100;
 *    ... (PLL calibration, TimeSync_Slew, TimeSync_PLLUpdate etc.) ...
 * }
 */
static int
TimeSyncSlewTime(TimeSyncData *data, int64 adjustment)
{
   (void)data;
   (void)adjustment;
   return 0;  /* Not implemented - caller falls back to step */
}
/* ROSE-TOOLS END */


/**
 * Reset slewing to nominal rate.
 */
static void
TimeSyncResetSlew(TimeSyncData *data)
{
   (void)data;
   /* ROSE-TOOLS BEGIN: TimeSync_PLLSetFrequency and TimeSync_DisableTimeSlew
    * are stubs in timeSyncWin.c for v0.2. */
   TimeSync_PLLSetFrequency(0);
   TimeSync_DisableTimeSlew();
   /* ROSE-TOOLS END */
}


/**
 * Update whether slewing is used for time correction.
 */
static void
TimeSyncSetSlewState(TimeSyncData *data, int active)
{
   if (active != data->slewActive) {
      if (!active) {
         TimeSyncResetSlew(data);
      }
      data->slewActive = active;
   }
}


/**
 * Set the guest OS time to the host OS time.
 */

/* ROSE-TOOLS BEGIN: gboolean -> int, Bool -> int */
static int
TimeSyncDoSyncWork(int slewCorrection,
                   TimeSyncType syncType,
                   int allowBackwardSync,
                   void *_data)
/* ROSE-TOOLS END */
{
   int64 guest, host;
   int64 gosError, apparentError, maxTimeError;
   /* ROSE-TOOLS BEGIN: Bool -> int */
   int apparentErrorValid;
   /* ROSE-TOOLS END */
   TimeSyncData *data = _data;

   if (!TimeSyncReadHostAndGuest(&host, &guest, &apparentError,
                                 &apparentErrorValid, &maxTimeError)) {
      return 0;
   }

   gosError = guest - host - apparentError;

   if (syncType == TIMESYNC_STEP || syncType == TIMESYNC_STEP_NORESYNC) {
      if (gosError < -maxTimeError ||
          (gosError > maxTimeError && allowBackwardSync)) {
         /* ROSE-TOOLS BEGIN: guestResync path removed - TimeSync_IsGuestSyncServiceRunning
          * always returns FALSE on Windows for v0.2, so we always take the
          * legacy step correction path. Original guestResync block preserved
          * as comment for v0.3 reference:
          *
          * if (syncType == TIMESYNC_STEP && data->guestResync &&
          *     TimeSync_IsGuestSyncServiceRunning()) {
          *    ... TimeSync_DoGuestResync / guestResyncTimer setup ...
          * } else {
          *    TimeSyncStepTime(data, -gosError + -apparentError);
          * }
          */
         if (!TimeSyncStepTime(data, -gosError + -apparentError)) {
            return 0;
         }
         /* ROSE-TOOLS END */
      }
   } else {
      /* ROSE-TOOLS BEGIN: v0.2 step-only path.
       * Upstream would slew when guest is ahead and apparentErrorValid.
       * Since TimeSync_Slew is not implemented for v0.2, we step in
       * both directions. Backward steps are safe on Windows since no
       * applications are sensitive to small backward jumps at startup.
       * TODO v0.3: restore slew path when TimeSync_Slew is implemented.
       *
       * Original:
       * if (gosError < -maxTimeError) {
       *    TimeSyncStepTime(data, -gosError + -apparentError);
       * } else if (slewCorrection && apparentErrorValid) {
       *    TimeSyncSlewTime(data, -gosError);
       * }
       */
      if (gosError < -maxTimeError || gosError > maxTimeError) {
         if (!TimeSyncStepTime(data, -gosError + -apparentError)) {
            return 0;
         }
      }
      /* ROSE-TOOLS END */
   }

   return 1;
}


/**
 * Check config and call TimeSyncDoSyncWork.
 */

/* ROSE-TOOLS BEGIN: gboolean -> int, VMTools_ConfigGetBoolean -> RoseConfig_GetBool */
static int
TimeSyncDoSync(int slewCorrection, TimeSyncType syncType,
               int allowBackwardSync, void *_data)
/* ROSE-TOOLS END */
{
   TimeSyncData *data = _data;
   /* ROSE-TOOLS BEGIN: VMTools_ConfigGetBoolean -> RoseConfig_GetBool */
   /* Original:
    * Bool disableAll = VMTools_ConfigGetBoolean(data->ctx->config,
    *                       CONFGROUPNAME_TIMESYNC, CONFNAME_TIMESYNC_DISABLE_ALL,
    *                       CONFNAME_TIMESYNC_DISABLE_ALL_DEFAULT);
    * Bool disablePeriodic = VMTools_ConfigGetBoolean(data->ctx->config,
    *                       CONFGROUPNAME_TIMESYNC, CONFNAME_TIMESYNC_DISABLE_PERIODIC,
    *                       CONFNAME_TIMESYNC_DISABLE_PERIODIC_DEFAULT);
    */
   int disableAll = RoseConfig_GetBool(data->ctx->config,
                                       ROSE_TIMESYNC_SECTION,
                                       ROSE_TIMESYNC_DISABLE_ALL, 0);
   int disablePeriodic = RoseConfig_GetBool(data->ctx->config,
                                            ROSE_TIMESYNC_SECTION,
                                            ROSE_TIMESYNC_DISABLE_PERIODIC, 0);
   /* ROSE-TOOLS END */

   if (disableAll || (disablePeriodic && syncType == TIMESYNC_PERIODIC)) {
      return 1;
   }
   return TimeSyncDoSyncWork(slewCorrection, syncType, allowBackwardSync, _data);
}


/* -----------------------------------------------------------------------
 * ROSE-TOOLS BEGIN: Signal handlers replacing GLib signal callbacks.
 * Original upstream used ToolsPluginSignalCb registered via VMTools_WrapArray.
 * We use RoseRegisterSignal in TimeSync_Init instead.
 * --------------------------------------------------------------------- */

static void
TimeSyncOnReset(void *src, void *_data)
{
   TimeSyncData *data = (TimeSyncData *)_data;
   (void)src;
   if (data != NULL && data->state == TIMESYNC_RUNNING) {
      TimeSyncDoSync(data->slewCorrection, TIMESYNC_STEP,
                     gTimeSyncToolsStartupAllowBackward, data);
   }
}

static void
TimeSyncOnShutdown(void *src, void *_data)
{
   TimeSyncData *data = (TimeSyncData *)_data;
   (void)src;
   if (data == NULL) {
      return;
   }
   if (data->state == TIMESYNC_RUNNING) {
      TimeSyncSetSlewState(data, 0);
      data->state = TIMESYNC_STOPPED;
   }
   free(data);
}

static void
TimeSyncOnSetOption(void *src, void *_data)
{
   /*
    * TODO v0.3: parse option/value from signal data and handle
    * TOOLSOPTION_SYNCTIME, TOOLSOPTION_SYNCTIME_PERIOD etc.
    * For v0.2 timesync runs unconditionally when the plugin is loaded.
    */
   (void)src;
   (void)_data;
}

/* ROSE-TOOLS END */


/* -----------------------------------------------------------------------
 * Plugin callbacks
 * --------------------------------------------------------------------- */

/* ROSE-TOOLS BEGIN: init/tick/shutdown replacing GLib timer loop */

static int
TimeSync_Init(RoseAppCtx *ctx)
{
   TimeSyncData *data;
   HANDLE        token;
   TOKEN_PRIVILEGES tp;
   LUID          luid;

   if (ctx->rpc == NULL) {
      fprintf(stderr, "timeSync: no RPC channel, refusing to load.\n");
      return 0;
   }

   /* Acquire SE_SYSTEMTIME_NAME privilege required for SetSystemTime */
   if (OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
      if (LookupPrivilegeValueA(NULL, SE_SYSTEMTIME_NAME, &luid)) {
         tp.PrivilegeCount           = 1;
         tp.Privileges[0].Luid       = luid;
         tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
         if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof tp,
                                    NULL, NULL) ||
             GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
            fprintf(stderr, "timeSync: failed to acquire SE_SYSTEMTIME_NAME "
                    "(err=%lu). SetSystemTime may fail.\n", GetLastError());
         }
      }
      CloseHandle(token);
   }

   data = (TimeSyncData *)malloc(sizeof *data);
   if (data == NULL) {
      fprintf(stderr, "timeSync: out of memory.\n");
      return 0;
   }

   memset(data, 0, sizeof *data);
   data->slewCorrection        = 0;
   data->slewPercentCorrection = TIMESYNC_PERCENT_CORRECTION;
   data->state                 = TIMESYNC_RUNNING;
   data->slewState             = TimeSyncUncalibrated;
   data->timeSyncPeriod        = TIMESYNC_TIME;
   data->guestResync           = 0;
   data->guestResyncTimeout    = 0;
   data->ctx                   = ctx;

   /* Register signal handlers */
   if (ctx->signals != NULL) {
      RoseRegisterSignal(ctx->signals, ROSE_SIG_RESET,
                         TimeSyncOnReset, data);
      RoseRegisterSignal(ctx->signals, ROSE_SIG_SET_OPTION,
                         TimeSyncOnSetOption, data);
      RoseRegisterSignal(ctx->signals, ROSE_SIG_SHUTDOWN,
                         TimeSyncOnShutdown, data);
   }

   /* Store data pointer in plugin private for tick/shutdown access */
   g_timeSyncData = data;

   return 1;
}

static int
TimeSync_Tick(RoseAppCtx *ctx)
{
   TimeSyncData *data = g_timeSyncData;
   if (data == NULL || data->state != TIMESYNC_RUNNING) {
      return 1;
   }
   TimeSyncDoSync(data->slewCorrection, TIMESYNC_PERIODIC, 0, data);
   return 1;
}

static void
TimeSync_Shutdown(RoseAppCtx *ctx)
{
   TimeSyncData *data = g_timeSyncData;
   if (data != NULL) {
      TimeSyncSetSlewState(data, 0);
      data->state = TIMESYNC_STOPPED;
      free(data);
      g_timeSyncData = NULL;
   }
}

/* ROSE-TOOLS END */


/* -----------------------------------------------------------------------
 * DllMain
 * --------------------------------------------------------------------- */
BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
   (void)hinstDLL;
   (void)fdwReason;
   (void)lpvReserved;
   return TRUE;
}


/* -----------------------------------------------------------------------
 * RoseOnLoad
 *
 * ROSE-TOOLS BEGIN: replaces ToolsOnLoad. Uses rose-tools plugin API
 * instead of GLib ToolsPluginData / VMTools_WrapArray.
 * Original ToolsOnLoad preserved as comment for reference:
 *
 * TOOLS_MODULE_EXPORT ToolsPluginData *
 * ToolsOnLoad(ToolsAppCtx *ctx) {
 *    static ToolsPluginData regData = { "timeSync", NULL, NULL };
 *    TimeSyncData *data = g_malloc(sizeof (TimeSyncData));
 *    RpcChannelCallback rpcs[] = { { TIMESYNC_SYNCHRONIZE, TimeSyncTcloHandler, ... } };
 *    ToolsPluginSignalCb sigs[] = { { TOOLS_CORE_SIG_SET_OPTION, ... }, { TOOLS_CORE_SIG_SHUTDOWN, ... } };
 *    ToolsAppReg regs[] = { { TOOLS_APP_GUESTRPC, ... }, { TOOLS_APP_SIGNALS, ... } };
 *    ... init data fields ...
 *    regData.regs = VMTools_WrapArray(regs, sizeof *regs, ARRAYSIZE(regs));
 *    g_timeSyncData = data;
 *    return &regData;
 * }
 * --------------------------------------------------------------------- */
ROSE_MODULE_EXPORT RosePluginData *
RoseOnLoad(RoseAppCtx *ctx)
{
   static RosePluginData regData;

   if (ctx->rpc == NULL) {
      return NULL;
   }

   memset(&regData, 0, sizeof regData);
   regData.name           = "timeSync";
   regData.init           = TimeSync_Init;
   regData.tick           = TimeSync_Tick;
   regData.shutdown       = TimeSync_Shutdown;
   regData.tickIntervalMs = TIMESYNC_TIME * 1000;

   return &regData;
}
/* ROSE-TOOLS END */
