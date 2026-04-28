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
 * rose-tools
 * services/checkvm/checkvm.c
 *
 * Detects whether we are running inside a VMware virtual machine
 * and reports the hypervisor version or product type.
 *
 * Usage:
 *   checkvm.exe      exit 0 and print version if inside a VM, else exit 1
 *   checkvm.exe -p   print product name (Workstation, ESX Server, etc.)
 *
 * Target: Windows NT 5.1 (XP) 32-bit, C89, MinGW
 */
#include <stdio.h>
#include <string.h>
#include "vm_basic_types.h"
#include "vm_vmx_type.h"
#include "vm_version.h"
#include "vmcheck.h"

#if defined(_WIN32)
#include <windows.h>

/*
 * Use CPUID instead of the VMware backdoor IN instruction for initial
 * detection. CPUID is unprivileged and safe on bare metal; the backdoor
 * faults on 64-bit Windows hosts when compiled with MinGW (no SEH).
 */
static int
CpuIdIsVMware(void)
{
    unsigned int eax, ebx, ecx, edx;

    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x1) :);

    if (!(ecx & (1u << 31))) {
        return 0;
    }

    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x40000000) :);

    return (ebx == 0x61774d56u &&
            ecx == 0x4d566572u &&
            edx == 0x65726177u);
}

#define SAFE_IS_VIRTUAL_WORLD() CpuIdIsVMware()
#else
#define SAFE_IS_VIRTUAL_WORLD() VmCheck_IsVirtualWorld()
#endif

int
main(int argc, char *argv[])
{
    uint32 version;
    uint32 type;
    int showProduct;

    showProduct = (argc == 2 && strcmp(argv[1], "-p") == 0);

    if (!SAFE_IS_VIRTUAL_WORLD()) {
        fprintf(stderr, "Not running inside a VMware virtual machine.\n");
        fflush(stderr);
        return 1;
    }

    if (!VmCheck_GetVersion(&version, &type)) {
        fprintf(stderr, "Failed to retrieve VMware version.\n");
        fflush(stderr);
        return 1;
    }

    if (showProduct) {
        switch (type) {
        case VMX_TYPE_SCALABLE_SERVER:
            printf("ESX Server\n");
            break;
        case VMX_TYPE_WORKSTATION:
            printf("Workstation\n");
            break;
        default:
            printf("Unknown\n");
            break;
        }
    } else {
        printf("%s version %lu (good)\n", PRODUCT_LINE_NAME, (unsigned long)version);
    }

    fflush(stdout);
    return 0;
}
