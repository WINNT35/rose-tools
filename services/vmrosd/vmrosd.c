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
 * Main daemon entry point. Initializes the application context and
 * delegates to mainLoop.c for setup, execution, and teardown.
 *
 * Mirrors open-vm-tools mainPosix.c - entry point only, no logic.
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vm_basic_types.h"
#include "vmware/tools/rose_plugin.h"

/* Forward declarations from mainLoop.c */
extern int  RoseCore_Setup(RoseAppCtx *ctx);
extern void RoseCore_Run(RoseAppCtx *ctx);

/* -----------------------------------------------------------------------
 * Global state
 * --------------------------------------------------------------------- */
static RoseAppCtx         g_ctx;
static RoseSignalRegistry g_signals;
static RoseConfig         g_config;

/* -----------------------------------------------------------------------
 * Console ctrl handler - clean shutdown on Ctrl+C
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
 * Context initialization
 * --------------------------------------------------------------------- */
static void
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
}

/* -----------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
   (void)argc;
   (void)argv;

   printf("vmrosd v0.1 starting.\n");

   SetConsoleCtrlHandler(CtrlHandler, TRUE);

   InitContext();

   if (!RoseCore_Setup(&g_ctx)) {
      return 1;
   }

   RoseCore_Run(&g_ctx);

   printf("vmrosd: stopped.\n");
   return g_ctx.errorCode;
}
