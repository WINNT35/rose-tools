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
 * rose-tools
 * services/vmrosd/serviceMain.c
 *
 * Windows Service Control Manager (SCM) integration for vmrosd.
 *
 * Provides:
 *   RoseService_Run()  -- called from vmrosd.c when running under SCM.
 *                         Registers ServiceMain with StartServiceCtrlDispatcher.
 *
 * SCM entry points (called by Windows, not by our code directly):
 *   ServiceMain()      -- SCM calls this to start the service.
 *   ServiceCtrlHandler() -- SCM calls this to stop/pause/continue.
 *
 * The service lifecycle:
 *   SCM calls ServiceMain()
 *     -> RegisterServiceCtrlHandlerEx()  registers ServiceCtrlHandler
 *     -> SetServiceStatus(START_PENDING)
 *     -> InitContext() + RoseCore_Setup()
 *     -> SetServiceStatus(RUNNING)
 *     -> RoseCore_Run()              blocks until ctx.running == 0
 *     -> SetServiceStatus(STOP_PENDING)
 *     -> SetServiceStatus(STOPPED)
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "vmware/tools/rose_plugin.h"

/* Service name as registered with SCM - must match sc create */
#define ROSE_SERVICE_NAME  "rose-tools"

/* Forward declarations from mainLoop.c */
extern int  RoseCore_Setup(RoseAppCtx *ctx);
extern void RoseCore_Run(RoseAppCtx *ctx);

/* Shared with vmrosd.c */
extern RoseAppCtx         g_ctx;
extern RoseSignalRegistry g_signals;
extern RoseConfig         g_config;
extern void               InitContext(void);

/* -----------------------------------------------------------------------
 * SCM state
 * --------------------------------------------------------------------- */
static SERVICE_STATUS_HANDLE g_statusHandle = NULL;
static SERVICE_STATUS        g_status;


/* -----------------------------------------------------------------------
 * ReportStatus
 *
 * Reports the current service state to SCM.
 * checkPoint: incremented value for START_PENDING/STOP_PENDING to show
 *             progress. Pass 0 for RUNNING/STOPPED.
 * waitHintMs: SCM timeout hint in ms. Pass 0 for RUNNING/STOPPED.
 * --------------------------------------------------------------------- */
static void
ReportStatus(DWORD state, DWORD exitCode,
             DWORD checkPoint, DWORD waitHintMs)
{
   g_status.dwCurrentState  = state;
   g_status.dwWin32ExitCode = exitCode;
   g_status.dwCheckPoint    = checkPoint;
   g_status.dwWaitHint      = waitHintMs;

   /* Only RUNNING and PAUSED accept control requests */
   if (state == SERVICE_RUNNING) {
      g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                    SERVICE_ACCEPT_SHUTDOWN;
   } else {
      g_status.dwControlsAccepted = 0;
   }

   SetServiceStatus(g_statusHandle, &g_status);
}


/* -----------------------------------------------------------------------
 * ServiceCtrlHandler
 *
 * Called by SCM when it wants to stop, pause, or query the service.
 * Must respond promptly -- do not block here.
 * --------------------------------------------------------------------- */
static DWORD WINAPI
ServiceCtrlHandler(DWORD control,
                   DWORD eventType,
                   LPVOID eventData,
                   LPVOID context)
{
   (void)eventType;
   (void)eventData;
   (void)context;

   switch (control) {
   case SERVICE_CONTROL_STOP:
   case SERVICE_CONTROL_SHUTDOWN:
      ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 1, 3000);
      g_ctx.running = 0;
      return NO_ERROR;

   case SERVICE_CONTROL_INTERROGATE:
      /* SCM asking for current status - ReportStatus was already called,
       * just return success so SCM knows we're responsive. */
      return NO_ERROR;

   default:
      return ERROR_CALL_NOT_IMPLEMENTED;
   }
}


/* -----------------------------------------------------------------------
 * ServiceMain
 *
 * Entry point called by SCM via StartServiceCtrlDispatcher.
 * argc/argv are service arguments (not used).
 * --------------------------------------------------------------------- */
static VOID WINAPI
ServiceMain(DWORD argc, LPTSTR *argv)
{
   (void)argc;
   (void)argv;

   /* Register control handler first - SCM requires this before any
    * other service status calls. */
   g_statusHandle = RegisterServiceCtrlHandlerEx(
      TEXT(ROSE_SERVICE_NAME),
      ServiceCtrlHandler,
      NULL);

   if (g_statusHandle == NULL) {
      /* Fatal - can't communicate with SCM.
       * Log the error to a file for diagnosis. */
      FILE *f = fopen("C:\\rose-tools-error.log", "w");
      if (f) {
         fprintf(f, "RegisterServiceCtrlHandlerEx failed: %lu\n",
                 GetLastError());
         fclose(f);
      }
      return;
   }

   /* Initialize status structure */
   memset(&g_status, 0, sizeof g_status);
   g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

   /* Tell SCM we are starting */
   ReportStatus(SERVICE_START_PENDING, NO_ERROR, 1, 10000);

   /* Initialize context */
   InitContext();
   ReportStatus(SERVICE_START_PENDING, NO_ERROR, 2, 10000);

   /* Set up VMware detection and RPC channel.
    * waitHint=10s gives plenty of room for backdoor init. */
   if (!RoseCore_Setup(&g_ctx)) {
      ReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0, 0);
      return;
   }
   ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3, 10000);

   /* We are running */
   ReportStatus(SERVICE_RUNNING, NO_ERROR, 0, 0);

   /* Main loop - blocks until SCM sends stop or an error occurs */
   RoseCore_Run(&g_ctx);

   /* Shut down cleanly */
   ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 1, 3000);
   ReportStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);
}


/* -----------------------------------------------------------------------
 * RoseService_Run
 *
 * Called from vmrosd.c main() when we detect we are running under SCM.
 * Hands control to StartServiceCtrlDispatcher which calls ServiceMain.
 *
 * Returns the exit code to pass back from main().
 * --------------------------------------------------------------------- */
int
RoseService_Run(void)
{
   SERVICE_TABLE_ENTRY dispatchTable[] = {
      { TEXT(ROSE_SERVICE_NAME), ServiceMain },
      { NULL,                    NULL        }
   };

   if (!StartServiceCtrlDispatcher(dispatchTable)) {
      return (int)GetLastError();
   }
   return 0;
}
