/** @file
  axl-webfs -- WebfsServerProtocol definition.

  The DXE serve driver registers this protocol on a fresh handle so the
  shell side (e.g. a future `serve-stop` verb) can find the running
  instance via axl_protocol_enumerate("webfs-server", ...) and read its
  port / mode / version.

  Stop is performed by axl_driver_unload on the driver image -- the
  driver's unload callback walks down through serve_core_teardown.
  The protocol struct is read-only state; no method pointers.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_SERVE_PROTOCOL_H_
#define AXL_WEBFS_SERVE_PROTOCOL_H_

#include <axl.h>
#include <stdint.h>

#define WEBFS_SERVER_PROTOCOL_NAME    "webfs-server"
#define WEBFS_SERVER_PROTOCOL_VERSION 1

/* Stable vendor GUID. Pinned at driver entry via
   axl_protocol_register_name so external consumers locating against
   this exact GUID and against the "webfs-server" name converge. */
#define WEBFS_SERVER_PROTOCOL_GUID \
    AXL_GUID(0x14b96243, 0x2a35, 0x4fae, \
             0xa0, 0xeb, 0xff, 0x63, 0xdb, 0x88, 0x23, 0x8e)

typedef struct {
    uint32_t version;             /* WEBFS_SERVER_PROTOCOL_VERSION */
    uint16_t port;
    char     mode[16];            /* "read-write" | "read-only" | "write-only" */
    uint8_t  addr[4];             /* bound IPv4 (after DHCP / static config) */
} WebfsServerProtocol;

#endif /* AXL_WEBFS_SERVE_PROTOCOL_H_ */
