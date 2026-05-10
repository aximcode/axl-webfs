/** @file
  axl-webfs -- shared serve descriptors (linked into both binaries).

  Both axl-webfs.efi (foreground launcher) and axl-webfs-serve-dxe.efi
  (resident driver) link this header so they agree on the ServeOpts
  layout, the AxlConfigDesc table, and the AxlService descriptor that
  drives both axl_service_run (foreground) and AXL_SERVICE_DRIVER
  (driver) shapes.

  Cross-binary ABI rule: build both binaries from the same source tree
  with identical compile flags. Per axl-sdk's AxlService contract.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_SERVE_SHARED_H_
#define AXL_WEBFS_SERVE_SHARED_H_

#include <axl.h>
#include <stddef.h>
#include <stdint.h>

/// Configuration + runtime state for the serve service.
///
/// The leading fields up through @ref source are populated by AxlConfig
/// auto-apply from @ref serve_descs (foreground from CLI args, driver
/// from LoadOptions). String fields are `const char *` per AxlConfig's
/// AXL_CFG_STRING contract — auto-apply assigns a pointer, not a copy,
/// so a buffer would silently truncate.
///
/// The trailing fields (@ref read_only ... @ref addr) are derived /
/// runtime, populated by @ref webfs_serve.setup. They live in the same
/// struct so route handlers and middleware can reach them via the
/// single `void *user` the AxlService threads through.
typedef struct {
    /* AxlConfig auto-apply targets */
    uint64_t    port;
    uint64_t    nic_index;             /* (uint64_t)-1 == auto */
    uint64_t    idle_timeout_sec;
    bool        verbose;
    const char *mode;                  /* "read-write" | "read-only" | "write-only" */
    const char *source;                /* bind IPv4, NULL/empty = auto */

    /* Derived / runtime, set by serve_setup */
    bool             read_only;
    bool             write_only;
    AxlHttpServer   *server;
    AxlLoop         *loop;             /* needed by route handlers to publish */
    uint32_t         request_sub_id;   /* axl_pubsub_subscribe handle */
    uint8_t          addr[4];
} ServeOpts;

/* Stable vendor GUID. AXL_SERVICE_DRIVER auto-publishes it on a
   sentinel handle so axl_service_is_running can detect a live
   instance. UUID v4 generated 2026-05-08. */
#define WEBFS_SERVICE_GUID \
    AXL_GUID(0x14b96243, 0x2a35, 0x4fae, \
             0xa0, 0xeb, 0xff, 0x63, 0xdb, 0x88, 0x23, 0x8e)

/* Defined in serve-core.c, linked into both binaries. */
extern ServeOpts          g_serve_opts;
extern const AxlConfigDesc serve_descs[];
extern const AxlService    webfs_serve;

#endif /* AXL_WEBFS_SERVE_SHARED_H_ */
