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
 * rose-tools
 * services/plugins/guestInfo/guestinfo.c
 *
 * Guest information plugin for vmrosd. Mirrors open-vm-tools guestInfoServer.c.
 * Periodically collects guest state and pushes updates to the VMX via RPC.
 * Updates are cache-gated so only changed data is transmitted.
 *
 * v0.1 scope:
 *   - OS name (short and full) via GetVersionEx
 *   - Hostname via GetComputerName
 *   - Primary IP via GetAdaptersInfo
 *   - Simple string cache to suppress redundant sends
 *   - Signal handlers: reset, shutdown, set_option (broadcastIP)
 *
 * TODO v0.2:
 *   - Full XDR NIC info with V3/V2/V1 fallback sequence
 *   - Disk info (JSON V1 and binary V0 fallback)
 *   - Memory/stats reporting (GuestInfo_StatProviderPoll equivalent)
 *   - Detailed OS data (INFO_OS_DETAILED / HostinfoDetailedDataHeader)
 *   - NIC exclude/primary/low-priority lists
 *   - GuestInfoCheckIfRunningSlow equivalent
 *   - GuestInfoVMSupport (vm-support script launch via CreateProcess)
 *   - Config-driven poll interval override
 *   - Uptime reporting (INFO_UPTIME)
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <iphlpapi.h>

#include "vm_basic_types.h"
#include "vmware/tools/rose_plugin.h"
#include "vmware/tools/guestrpc.h"

/* Poll interval in milliseconds - mirrors GUESTINFO_POLL_INTERVAL (30s) */
#define GUESTINFO_POLL_INTERVAL_MS  (30 * 1000)

/* GUEST_INFO_COMMAND mirrors the upstream RPC command prefix */
#define GUEST_INFO_COMMAND          "SetGuestInfo"

/* GuestInfoType values - mirrors guestInfo.h */
#define INFO_DNS_NAME               1
#define INFO_OS_NAME                2
#define INFO_OS_NAME_FULL           3

/* Simple string cache - mirrors GuestInfoCache.value[] */
static char *g_cachedDnsName  = NULL;
static char *g_cachedOsName   = NULL;
static char *g_cachedOsFull   = NULL;
static char *g_cachedIp       = NULL;

/* Whether VM was resumed - triggers full cache clear on next gather */
static int g_vmResumed = 0;

/* -----------------------------------------------------------------------
 * GuestInfoClearCache
 *
 * Resets all cached values. Mirrors GuestInfoClearCache.
 * --------------------------------------------------------------------- */
static void
GuestInfoClearCache(void)
{
   free(g_cachedDnsName);  g_cachedDnsName = NULL;
   free(g_cachedOsName);   g_cachedOsName  = NULL;
   free(g_cachedOsFull);   g_cachedOsFull  = NULL;
   free(g_cachedIp);       g_cachedIp      = NULL;
}

/* -----------------------------------------------------------------------
 * SetGuestInfo
 *
 * Sends a key-value pair to the VMX.
 * Mirrors SetGuestInfo in guestInfoServer.c.
 * Format: "SetGuestInfo <key> <value>"
 * --------------------------------------------------------------------- */
static int
SetGuestInfo(RoseAppCtx *ctx, int key, const char *value)
{
   char    buf[1024];
   char   *reply   = NULL;
   size_t  repLen  = 0;
   int     status;

   if (ctx->rpc == NULL || value == NULL) {
      return 0;
   }

   _snprintf(buf, sizeof buf, "%s  %d %s",
             GUEST_INFO_COMMAND, key, value);
   buf[sizeof buf - 1] = '\0';

   status = (int)RpcChannel_Send(ctx->rpc, buf, strlen(buf) + 1,
                                 &reply, &repLen);

   if (status && reply != NULL) {
      status = (reply[0] == '\0');
   }

   free(reply);
   return status;
}

/* -----------------------------------------------------------------------
 * GetOsName
 *
 * Gets the short OS name string for INFO_OS_NAME.
 * Mirrors Hostinfo_GetOSGuestString on Windows.
 * --------------------------------------------------------------------- */
static void
GetOsName(char *buf, size_t bufsz)
{
   OSVERSIONINFOEXA ovi;
   ZeroMemory(&ovi, sizeof ovi);
   ovi.dwOSVersionInfoSize = sizeof ovi;
   GetVersionExA((LPOSVERSIONINFOA)&ovi);

   if (ovi.dwMajorVersion == 5 && ovi.dwMinorVersion == 1) {
      _snprintf(buf, bufsz, "winXPPro");
   } else if (ovi.dwMajorVersion == 5 && ovi.dwMinorVersion == 2) {
      _snprintf(buf, bufsz, "win2003srv");
   } else if (ovi.dwMajorVersion == 6 && ovi.dwMinorVersion == 0) {
      _snprintf(buf, bufsz, "winVista");
   } else if (ovi.dwMajorVersion == 6 && ovi.dwMinorVersion == 1) {
      _snprintf(buf, bufsz, "windows7srv");
   } else {
      _snprintf(buf, bufsz, "winUnknown");
   }
   buf[bufsz - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * GetOsNameFull
 *
 * Gets the full OS name string for INFO_OS_NAME_FULL.
 * Mirrors Hostinfo_GetOSName on Windows.
 * --------------------------------------------------------------------- */
static void
GetOsNameFull(char *buf, size_t bufsz)
{
   OSVERSIONINFOEXA ovi;
   ZeroMemory(&ovi, sizeof ovi);
   ovi.dwOSVersionInfoSize = sizeof ovi;
   GetVersionExA((LPOSVERSIONINFOA)&ovi);

   if (ovi.dwMajorVersion == 5 && ovi.dwMinorVersion == 1) {
      _snprintf(buf, bufsz, "Microsoft Windows XP Professional SP%d",
                ovi.wServicePackMajor);
   } else if (ovi.dwMajorVersion == 5 && ovi.dwMinorVersion == 2) {
      _snprintf(buf, bufsz, "Microsoft Windows Server 2003 SP%d",
                ovi.wServicePackMajor);
   } else if (ovi.dwMajorVersion == 6 && ovi.dwMinorVersion == 0) {
      _snprintf(buf, bufsz, "Microsoft Windows Vista SP%d",
                ovi.wServicePackMajor);
   } else if (ovi.dwMajorVersion == 6 && ovi.dwMinorVersion == 1) {
      _snprintf(buf, bufsz, "Microsoft Windows 7 SP%d",
                ovi.wServicePackMajor);
   } else {
      _snprintf(buf, bufsz, "Microsoft Windows %lu.%lu SP%d",
                ovi.dwMajorVersion, ovi.dwMinorVersion,
                ovi.wServicePackMajor);
   }
   buf[bufsz - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * GetPrimaryIp
 *
 * Gets the primary IPv4 address via GetAdaptersInfo.
 * Mirrors GuestInfo_GetPrimaryIP on Windows.
 * --------------------------------------------------------------------- */
static void
GetPrimaryIp(char *buf, size_t bufsz)
{
   IP_ADAPTER_INFO  adapterInfo[16];
   DWORD            bufLen = sizeof adapterInfo;
   DWORD            status;
   IP_ADAPTER_INFO *adapter;

   buf[0] = '\0';

   status = GetAdaptersInfo(adapterInfo, &bufLen);
   if (status != ERROR_SUCCESS) {
      return;
   }

   for (adapter = adapterInfo; adapter != NULL; adapter = adapter->Next) {
      const char *ip = adapter->IpAddressList.IpAddress.String;

      /* Skip loopback and unassigned */
      if (strcmp(ip, "0.0.0.0") == 0 ||
          strcmp(ip, "127.0.0.1") == 0) {
         continue;
      }

      strncpy(buf, ip, bufsz - 1);
      buf[bufsz - 1] = '\0';
      return;
   }
}

/* -----------------------------------------------------------------------
 * GuestInfoGather
 *
 * Collects guest state and sends updates to VMX if changed.
 * Mirrors GuestInfoGather — stripped to v0.1 scope.
 * --------------------------------------------------------------------- */
static void
GuestInfoGather(RoseAppCtx *ctx)
{
   char osName[256];
   char osFull[256];
   char dnsName[256];
   char ip[64];

   if (ctx->rpc == NULL) {
      return;
   }

   /* Clear cache if VM was resumed */
   if (g_vmResumed) {
      g_vmResumed = 0;
      GuestInfoClearCache();
   }

   /* OS short name */
   GetOsName(osName, sizeof osName);
   if (g_cachedOsName == NULL ||
       strcmp(g_cachedOsName, osName) != 0) {
      if (SetGuestInfo(ctx, INFO_OS_NAME, osName)) {
         free(g_cachedOsName);
         g_cachedOsName = _strdup(osName);
      }
   }

   /* OS full name */
   GetOsNameFull(osFull, sizeof osFull);
   if (g_cachedOsFull == NULL ||
       strcmp(g_cachedOsFull, osFull) != 0) {
      if (SetGuestInfo(ctx, INFO_OS_NAME_FULL, osFull)) {
         free(g_cachedOsFull);
         g_cachedOsFull = _strdup(osFull);
      }
   }

   /* Hostname */
   dnsName[0] = '\0';
   {
      DWORD sz = sizeof dnsName;
      GetComputerNameA(dnsName, &sz);
   }
   if (dnsName[0] != '\0' &&
       (g_cachedDnsName == NULL ||
        strcmp(g_cachedDnsName, dnsName) != 0)) {
      if (SetGuestInfo(ctx, INFO_DNS_NAME, dnsName)) {
         free(g_cachedDnsName);
         g_cachedDnsName = _strdup(dnsName);
      }
   }

   /* Primary IP - send as info-set guestinfo.ip */
   GetPrimaryIp(ip, sizeof ip);
   if (ip[0] != '\0' &&
       (g_cachedIp == NULL ||
        strcmp(g_cachedIp, ip) != 0)) {
      char buf[128];
      char *reply   = NULL;
      size_t repLen = 0;

      _snprintf(buf, sizeof buf, "info-set guestinfo.ip %s", ip);
      buf[sizeof buf - 1] = '\0';

      if (RpcChannel_Send(ctx->rpc, buf, strlen(buf) + 1,
                          &reply, &repLen)) {
         free(g_cachedIp);
         g_cachedIp = _strdup(ip);
      }
      free(reply);
   }

   /* TODO v0.2: disk info */
   /* TODO v0.2: memory/stats */
   /* TODO v0.2: uptime */
   /* TODO v0.2: full XDR NIC info */
}

/* -----------------------------------------------------------------------
 * Signal handlers
 * --------------------------------------------------------------------- */

static void
GuestInfoServerReset(void *src, void *data)
{
   (void)src;
   (void)data;
   g_vmResumed = 1;
}

static void
GuestInfoServerShutdown(void *src, void *data)
{
   (void)src;
   (void)data;
   GuestInfoClearCache();
   /* TODO v0.2: GuestInfo_StatProviderShutdown equivalent */
   /* TODO v0.2: NetUtil_FreeIpHlpApiDll equivalent */
}

static void
GuestInfoServerSetOption(void *src, void *data)
{
   /*
    * TODO v0.2: parse option/value from signal data and handle
    * TOOLSOPTION_BROADCASTIP properly. For v0.1 the IP is always
    * sent during the gather tick regardless.
    */
   (void)src;
   (void)data;
}

static void
GuestInfoServerSendCaps(void *src, void *data)
{
   /*
    * TODO v0.2: send uptime when capabilities are set.
    * Mirrors GuestInfoServerSendCaps which sends INFO_UPTIME
    * as a legacy VMX expectation during capability registration.
    */
   (void)src;
   (void)data;
}

/* -----------------------------------------------------------------------
 * Plugin tick
 *
 * Called by pluginMgr on the configured interval.
 * --------------------------------------------------------------------- */
static int
GuestInfo_Tick(RoseAppCtx *ctx)
{
   GuestInfoGather(ctx);
   return 1;
}

/* -----------------------------------------------------------------------
 * Plugin init
 * --------------------------------------------------------------------- */
static int
GuestInfo_Init(RoseAppCtx *ctx)
{
   if (ctx->rpc == NULL) {
      fprintf(stderr, "guestinfo: no RPC channel, refusing to load.\n");
      return 0;
   }

   /* Register signal handlers */
   if (ctx->signals != NULL) {
      RoseRegisterSignal(ctx->signals, ROSE_SIG_RESET,
                         GuestInfoServerReset, NULL);
      RoseRegisterSignal(ctx->signals, ROSE_SIG_SHUTDOWN,
                         GuestInfoServerShutdown, NULL);
      RoseRegisterSignal(ctx->signals, ROSE_SIG_SET_OPTION,
                         GuestInfoServerSetOption, NULL);
      RoseRegisterSignal(ctx->signals, ROSE_SIG_CAPABILITIES,
                         GuestInfoServerSendCaps, NULL);
   }

   GuestInfoClearCache();
   g_vmResumed = 0;

   /* Do an immediate gather on init */
   GuestInfoGather(ctx);

   return 1;
}

/* -----------------------------------------------------------------------
 * Plugin shutdown
 * --------------------------------------------------------------------- */
static void
GuestInfo_Shutdown(RoseAppCtx *ctx)
{
   (void)ctx;
   GuestInfoClearCache();
}

/* -----------------------------------------------------------------------
 * GuestInfo_OnLoad
 *
 * Plugin entry point. Mirrors ToolsOnLoad.
 * Returns NULL if no RPC channel exists.
 * --------------------------------------------------------------------- */
RosePluginData *
GuestInfo_OnLoad(RoseAppCtx *ctx)
{
   static RosePluginData regData;

   if (ctx->rpc == NULL) {
      return NULL;
   }

   memset(&regData, 0, sizeof regData);
   regData.name           = "guestInfo";
   regData.init           = GuestInfo_Init;
   regData.tick           = GuestInfo_Tick;
   regData.shutdown       = GuestInfo_Shutdown;
   regData.tickIntervalMs = GUESTINFO_POLL_INTERVAL_MS;

   return &regData;
}
