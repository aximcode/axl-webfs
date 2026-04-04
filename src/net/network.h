/** @file
  NetworkLib -- NIC discovery, DHCP configuration, and static IP setup.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
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

int  network_init(size_t nic_index, const uint8_t *static_ip, size_t timeout_sec);
int  network_get_address(uint8_t ip_out[4]);
int  network_get_info(NetworkInfo *info);
void network_cleanup(void);

#endif
