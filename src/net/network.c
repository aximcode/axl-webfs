/** @file
  NetworkLib -- NIC discovery, DHCP configuration, and static IP setup.

  Uses axl_net_auto_init() for DHCP and axl_net_set_static_ip() for
  static IP. No direct UEFI protocol calls.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "net/network.h"

#include <axl.h>
#include <axl/axl-net.h>

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static bool         mInitialized = false;
static NetworkInfo  mIfaceInfo;

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int
network_init(size_t nic_index, const uint8_t *static_ip, size_t timeout_sec)
{
    if (mInitialized) return -1;

    axl_memset(&mIfaceInfo, 0, sizeof(mIfaceInfo));

    if (static_ip == NULL) {
        if (axl_net_auto_init(nic_index, timeout_sec) != 0) {
            axl_printf("ERROR: Network init failed\n");
            return -1;
        }
    } else {
        /* Load drivers and connect SNP, then set static IP */
        axl_net_auto_init(nic_index, 0);

        uint8_t netmask[] = {255, 255, 255, 0};
        if (axl_net_set_static_ip(nic_index, static_ip, netmask, NULL) != 0) {
            axl_printf("ERROR: Failed to set static IP\n");
            return -1;
        }
        axl_stall(500000);
    }

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
