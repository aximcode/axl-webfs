/** @file
  NetworkLib -- NIC discovery, DHCP configuration, and static IP setup.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef NETWORK_LIB_H_
#define NETWORK_LIB_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t  ip[4];
    uint8_t  subnet_mask[4];
    uint8_t  gateway[4];
    uint8_t  mac[6];
    uint32_t mac_len;
    bool     valid;
} NetworkInfo;

/// Bring up the requested NIC. @p nic_index uses (uint64_t)-1 to
/// mean "auto-detect" so callers can pass an AxlConfig-managed uint
/// straight through without a per-caller sentinel translation.
/// @p static_ip is NULL for DHCP, non-NULL to assign the 4-byte
/// IPv4 statically. @p timeout_sec bounds DHCP / link-up waits.
int  network_init(uint64_t nic_index, const uint8_t *static_ip, size_t timeout_sec);
int  network_get_address(uint8_t ip_out[4]);
int  network_get_info(NetworkInfo *info);
void network_cleanup(void);

#endif
