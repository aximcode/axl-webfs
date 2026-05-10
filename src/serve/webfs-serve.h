/** @file
  axl-webfs -- serve service descriptor (linked into both binaries).

  webfs-serve.c defines the descriptor + ServeOpts unconditionally,
  but the impl (setup, teardown, route handlers, helpers) is gated
  on AXL_SERVICE_BUILD_DRIVER so the launcher build only carries the
  descriptor stub. The launcher reads svc->opts_descs / svc->user
  for axl_service_start_embedded's LoadOptions serialization; it
  never invokes setup/teardown (those run on the driver side).

  Cross-binary ABI rule: build both binaries from the same source
  tree with identical compile flags except for the
  -DAXL_SERVICE_BUILD_DRIVER toggle. Per axl-sdk's AxlService
  contract.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_SERVE_H_
#define AXL_WEBFS_SERVE_H_

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
    const char *log_path;              /* file path, empty = console only */

    /* Derived / runtime, set by serve_setup */
    bool             read_only;
    bool             write_only;
    AxlHttpServer   *server;
    AxlLoop         *loop;             /* needed by route handlers to publish */
    uint32_t         request_sub_id;   /* axl_pubsub_subscribe handle */
    AxlStream       *log_stream;       /* tee target for stdout/stderr; NULL = no log file */
    uint8_t          addr[4];
} ServeOpts;

/* The service's UEFI protocol identity is derived from
   webfs_serve.name via axl_guid_v5 (see axl-sdk's axl-service.h).
   AXL_SERVICE_DRIVER publishes the derived GUID on the driver
   image's handle so axl_service_is_running can detect a live
   instance — both binaries pick out the same derived GUID by
   sharing this descriptor. */

/* Defined in webfs-serve.c, linked into both binaries. */
extern ServeOpts          g_serve_opts;
extern const AxlConfigDesc serve_descs[];
extern const AxlService    webfs_serve;

#endif /* AXL_WEBFS_SERVE_H_ */
