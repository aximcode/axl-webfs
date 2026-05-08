/** @file
  axl-webfs -- serve core (shared between foreground app and DXE driver).

  Wraps the bring-up of the HTTP file server: network init, FT volume
  enumeration, AxlHttpServer creation, route registration, attach to an
  AxlLoop. Callers drive the loop (axl_loop_run for foreground, timer-
  driven axl_loop_dispatch for the resident driver) and call
  serve_core_teardown() before unloading.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_SERVE_CORE_H_
#define AXL_WEBFS_SERVE_CORE_H_

#include <axl.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t    port;
    size_t      nic_index;            /* (size_t)-1 = auto */
    size_t      idle_timeout_sec;
    bool        read_only;
    bool        write_only;
    bool        verbose;
    const char *source;               /* bind ipv4, NULL = auto */
} ServeCoreOpts;

/// HTTP server bring-up state. Lifetime: callers MUST keep ServeCore
/// alive for the entire run of the loop -- the permission middleware
/// and the path handler hold @p &core->opts and dereference it on every
/// request. A stack-local ServeCore is fine for the foreground command
/// (lives until @ref axl_loop_run returns); driver-image users should
/// use file-static or heap-allocated storage.
typedef struct {
    AxlHttpServer *server;
    AxlLoop       *loop;
    ServeCoreOpts  opts;              /* stable copy used by middleware */
    uint8_t        addr[4];           /* bound IPv4 (after DHCP) */
} ServeCore;

/// Bring the HTTP server up, register every route, and attach it to a
/// fresh AxlLoop. Guarantees @ref ft_init has run on success, so callers
/// can use ft_get_volume_count / ft_get_volume immediately afterwards.
///
/// On success @p core is fully initialised and the caller MUST pair it
/// with @ref serve_core_teardown. On failure setup self-cleans (releases
/// any partial bring-up); the caller MUST NOT call teardown.
///
/// @return AXL_OK on success, AXL_ERR on failure.
int  serve_core_setup(const ServeCoreOpts *opts, ServeCore *core);

/// Tear down a server brought up by a successful @ref serve_core_setup.
/// Frees the HTTP server (which detaches its event sources from the
/// loop), then frees the loop, then runs network_cleanup.
void serve_core_teardown(ServeCore *core);

#endif /* AXL_WEBFS_SERVE_CORE_H_ */
