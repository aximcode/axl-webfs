/** @file
  NetworkLib -- NIC discovery, DHCP configuration, and static IP setup.

  Uses axl_net_auto_init() for the common DHCP path. Retains
  static IP support via direct IP4Config2 protocol calls.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/NetworkLib.h>

#include <axl.h>
#include <axl/axl-driver.h>
#include <axl/axl-sys.h>
#include <axl/axl-net.h>
#include <uefi/axl-uefi.h>

/* ------------------------------------------------------------------ */
/* IP4Config2 types not in the generated UEFI headers                 */
/* ------------------------------------------------------------------ */

typedef enum {
    Ip4Config2PolicyStatic = 0,
    Ip4Config2PolicyDhcp   = 1
} EFI_IP4_CONFIG2_POLICY;

typedef struct {
    EFI_IPv4_ADDRESS  Address;
    EFI_IPv4_ADDRESS  SubnetMask;
} EFI_IP4_CONFIG2_MANUAL_ADDRESS;

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static bool         mInitialized = false;
static NetworkInfo  mIfaceInfo;

/* ------------------------------------------------------------------ */
/* Static IP helper (only used when caller passes a static IP)        */
/* ------------------------------------------------------------------ */

static int
configure_static_ip(void *handle, const uint8_t *static_ip)
{
    EFI_IP4_CONFIG2_PROTOCOL *ip4cfg2 = NULL;
    if (axl_handle_get_service(handle, "ip4-config2", (void **)&ip4cfg2) != 0)
        return -1;

    EFI_IP4_CONFIG2_POLICY static_policy = Ip4Config2PolicyStatic;
    EFI_STATUS status = ip4cfg2->SetData(
        ip4cfg2, Ip4Config2DataTypePolicy,
        sizeof(EFI_IP4_CONFIG2_POLICY), &static_policy);
    if (EFI_ERROR(status)) return -1;

    EFI_IP4_CONFIG2_MANUAL_ADDRESS manual_addr;
    axl_memset(&manual_addr, 0, sizeof(manual_addr));
    axl_memcpy(&manual_addr.Address, static_ip, 4);
    manual_addr.SubnetMask.Addr[0] = 255;
    manual_addr.SubnetMask.Addr[1] = 255;
    manual_addr.SubnetMask.Addr[2] = 255;
    manual_addr.SubnetMask.Addr[3] = 0;

    status = ip4cfg2->SetData(
        ip4cfg2, Ip4Config2DataTypeManualAddress,
        sizeof(manual_addr), &manual_addr);
    return EFI_ERROR(status) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int
network_init(size_t nic_index, const uint8_t *static_ip, size_t timeout_sec)
{
    if (mInitialized) return -1;

    axl_memset(&mIfaceInfo, 0, sizeof(mIfaceInfo));

    if (static_ip == NULL) {
        /* Standard DHCP path — SDK handles everything */
        if (axl_net_auto_init(nic_index, timeout_sec) != 0) {
            axl_printf("ERROR: Network init failed\n");
            return -1;
        }
    } else {
        /* Static IP: auto-init loads drivers and connects SNP,
           then we configure the IP manually */
        axl_net_auto_init(nic_index, 0);

        void **ip4_handles = NULL;
        size_t ip4_count = 0;
        if (axl_service_enumerate("ip4-config2", &ip4_handles, &ip4_count) != 0 ||
            ip4_count == 0) {
            axl_free(ip4_handles);
            axl_printf("ERROR: No network interface found\n");
            return -1;
        }
        void *ip4_handle = ip4_handles[0];
        axl_free(ip4_handles);

        if (configure_static_ip(ip4_handle, static_ip) != 0) {
            axl_printf("ERROR: Failed to set static IP\n");
            return -1;
        }
        axl_stall(500000);
    }

    /* Read the acquired IP */
    AxlIPv4Address ip;
    if (axl_net_get_ip_address(&ip) != 0) {
        axl_printf("ERROR: No IP address acquired\n");
        return -1;
    }
    mIfaceInfo.valid = true;
    axl_memcpy(mIfaceInfo.ip, ip.addr, 4);

    mInitialized = true;
    return 0;
}

int
network_get_address(uint8_t ip_out[4])
{
    if (!mInitialized || !mIfaceInfo.valid) return -1;
    axl_memcpy(ip_out, mIfaceInfo.ip, 4);
    return 0;
}

int
network_get_info(NetworkInfo *info)
{
    if (!mInitialized || !mIfaceInfo.valid) return -1;
    axl_memcpy(info, &mIfaceInfo, sizeof(NetworkInfo));
    return 0;
}

void
network_cleanup(void)
{
    axl_memset(&mIfaceInfo, 0, sizeof(mIfaceInfo));
    mInitialized = false;
}
