/** @file
  NetworkLib -- thin idempotency + state cache around axl_net_bring_up.

  axl-sdk does the actual work: drivers up, DHCP or static, address
  read-back. We just cache the resulting address so callers can ask
  for it later without re-querying the protocol.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
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
network_init(uint64_t nic_index, const uint8_t *static_ip, size_t timeout_sec)
{
    if (mInitialized) return -1;

    axl_memset(&mIfaceInfo, 0, sizeof(mIfaceInfo));

    /* Sentinel translation in one place: AxlConfig descriptors
       represent "auto-detect" as (uint64_t)-1 so the value can ride
       through LoadOptions / AxlArgs unchanged; the SDK's bring_up
       uses (size_t)-1 for the same meaning. Doing the conversion
       here keeps every caller out of the sentinel-mapping business
       (DRY: serve_setup, mount_setup were both doing it). */
    size_t nic = (nic_index == (uint64_t)-1)
                 ? (size_t)-1
                 : (size_t)nic_index;

    /* axl_net_bring_up: NULL static_ip -> DHCP; non-NULL -> static
       (default netmask 255.255.255.0, no gateway). Returns the
       acquired address in addr_out. */
    AxlIPv4Address addr;
    if (axl_net_bring_up(nic, static_ip, NULL, NULL,
                         timeout_sec, &addr) != 0) {
        axl_printf("ERROR: Network init failed\n");
        return -1;
    }

    mIfaceInfo.valid = true;
    axl_memcpy(mIfaceInfo.ip, addr.addr, 4);
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
