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
 * services/vmrosd/vmrosd.c
 *
 * Main entry point for vmrosd. Detects whether it is being launched
 * by the Windows Service Control Manager (SCM) or run interactively
 * from a terminal, and dispatches accordingly.
 *
 * Interactive:  SetConsoleCtrlHandler + RoseCore_Setup + RoseCore_Run
 * Service:      RoseService_Run() in serviceMain.c handles everything
 *
 * The same binary works in both modes - no separate service binary needed.
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "vmware/tools/rose_plugin.h"
#include "roseConfig.h"

/* Forward declarations from mainLoop.c */
extern int  RoseCore_Setup(RoseAppCtx *ctx);
extern void RoseCore_Run(RoseAppCtx *ctx);

/* Forward declaration from serviceMain.c */
extern int  RoseService_Run(void);

/* -----------------------------------------------------------------------
 * Global state - shared with serviceMain.c
 * --------------------------------------------------------------------- */
RoseAppCtx         g_ctx;
RoseSignalRegistry g_signals;
RoseConfig         g_config;

/* -----------------------------------------------------------------------
 * InitContext
 *
 * Shared with serviceMain.c - called from both interactive and service
 * paths to zero and populate the application context.
 * --------------------------------------------------------------------- */
void
InitContext(void)
{
   memset(&g_ctx,     0, sizeof g_ctx);
   memset(&g_signals, 0, sizeof g_signals);
   memset(&g_config,  0, sizeof g_config);

   g_ctx.version  = ROSE_CORE_API_V1;
   g_ctx.name     = ROSE_GUEST_SERVICE;
   g_ctx.running  = 1;
   g_ctx.config   = &g_config;
   g_ctx.signals  = &g_signals;

   /* Load config file - missing file is not an error, all defaults apply */
   RoseConfig_Load(&g_config, ROSE_CONF_PATH);
   if (g_config.sectionCount > 0) {
      fprintf(stdout, "vmrosd: config loaded from %s (%d section(s))\n",
              ROSE_CONF_PATH, g_config.sectionCount);
   } else {
      fprintf(stdout, "vmrosd: no config file found, using defaults.\n");
   }
}

/* -----------------------------------------------------------------------
 * CtrlHandler
 *
 * Console Ctrl+C / close handler for interactive mode only.
 * Not used when running as a service - SCM sends SERVICE_CONTROL_STOP
 * to ServiceCtrlHandler in serviceMain.c instead.
 * --------------------------------------------------------------------- */
static BOOL WINAPI
CtrlHandler(DWORD ctrlType)
{
   switch (ctrlType) {
   case CTRL_C_EVENT:
   case CTRL_BREAK_EVENT:
   case CTRL_CLOSE_EVENT:
      printf("vmrosd: shutting down...\n");
      g_ctx.running = 0;
      return TRUE;
   default:
      return FALSE;
   }
}

/* -----------------------------------------------------------------------
 * RunInteractive
 *
 * Runs vmrosd as a normal console application.
 * Used when launched from a terminal for debugging.
 * --------------------------------------------------------------------- */
static int
RunInteractive(void)
{
   printf("vmrosd v0.2 starting (interactive mode).\n");

   SetConsoleCtrlHandler(CtrlHandler, TRUE);

   InitContext();

   if (!RoseCore_Setup(&g_ctx)) {
      return 1;
   }

   RoseCore_Run(&g_ctx);

   printf("vmrosd: stopped.\n");
   return g_ctx.errorCode;
}

/* -----------------------------------------------------------------------
 * main
 *
 * Detects launch context and dispatches:
 *
 *   SCM launch:         StartServiceCtrlDispatcher succeeds.
 *   Interactive launch: StartServiceCtrlDispatcher fails with
 *                       ERROR_FAILED_SERVICE_CONTROLLER_CONNECT,
 *                       meaning we were not started by SCM.
 *
 * This detection is the standard Windows pattern for a binary that
 * works both as a service and as a console application.
 * --------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
   int ret;

   (void)argc;
   (void)argv;

   /* --- VMware Tools detection ---
    * Must be the very first check - before SCM registration, context
    * init, or any RPC work. Scanning Program Files is instant and
    * avoids any channel conflict issues entirely.
    */
   {
      char vmtoolsPath[MAX_PATH];
      DWORD pfLen = GetEnvironmentVariableA("ProgramW6432",
                                             vmtoolsPath,
                                             sizeof vmtoolsPath);
      /* ProgramW6432 is only set on 64-bit Windows. On a genuine 32-bit OS
       * it will be absent, so fall back to plain ProgramFiles. */
      if (pfLen == 0 || pfLen >= sizeof vmtoolsPath) {
         pfLen = GetEnvironmentVariableA("ProgramFiles",
                                          vmtoolsPath,
                                          sizeof vmtoolsPath);
      }
      if (pfLen > 0 && pfLen < sizeof vmtoolsPath) {
         strncat(vmtoolsPath, "\\VMware\\VMware Tools",
                 sizeof vmtoolsPath - strlen(vmtoolsPath) - 1);
         vmtoolsPath[sizeof vmtoolsPath - 1] = '\0';
         if (GetFileAttributesA(vmtoolsPath) != INVALID_FILE_ATTRIBUTES) {
            RoseConfig earlyCfg;
            memset(&earlyCfg, 0, sizeof earlyCfg);
            RoseConfig_Load(&earlyCfg, ROSE_CONF_PATH);
            if (RoseConfig_GetBool(&earlyCfg, "vmtools",
                                   "ignore_vmware_tools", 0)) {
               fprintf(stderr,
                       "vmrosd: warning: VMware Tools installation found; "
                       "continuing because ignore_vmware_tools is set.\n");
            } else {
               fprintf(stderr,
                       "An installation of VMware Tools has been detected. "
                       "Rose-Tools cannot initialize while VMware Tools is "
                       "running. Aborting.\n");
               return 1;
            }
         }
      }
   }

   ret = RoseService_Run();

   if (ret == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      /*
       * Not started by SCM - run interactively.
       * This is the normal path when debugging from a terminal.
       */
      return RunInteractive();
   }

   /*
    * Started by SCM - RoseService_Run() handled everything.
    * ret is 0 on clean exit, or a Win32 error code on failure.
    */
   return ret;
}
