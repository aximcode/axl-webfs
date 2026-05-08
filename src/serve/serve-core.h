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

// ----------------------------------------------------------------------------
// Load-options serialisation
//
// The foreground `serve --detach` verb packs ServeCoreOpts into a UTF-8
// `key=value;key=value;...` string and ships it to the driver image as
// LoadOptions (UCS-2). The driver's DriverEntry parses the string back
// into a ServeCoreOpts. Both sides MUST agree on the encoding -- having
// it here keeps them in sync.
// ----------------------------------------------------------------------------

/// Serialise @p opts into @p out (UTF-8). @p source_buf must outlive the
/// resulting ServeCoreOpts on the driver side; pass NULL to skip the
/// source field.
///
/// @return AXL_OK on success, AXL_ERR if @p out_size is too small.
int  serve_opts_serialize(const ServeCoreOpts *opts,
                          char *out, size_t out_size);

/// Parse a UTF-8 key=value;... string into @p opts. @p source_buf is a
/// caller-owned buffer of at least @p source_buf_size bytes; if the
/// load-options string contains a `source=` key its value is copied
/// there and @p opts->source is pointed at the buffer. If absent
/// @p opts->source is NULL.
///
/// Unknown keys are silently ignored to keep this forward-compatible
/// with future foreground-app additions.
///
/// @return AXL_OK on success, AXL_ERR on a malformed input.
int  serve_opts_parse(const char *in, ServeCoreOpts *opts,
                      char *source_buf, size_t source_buf_size);

#endif /* AXL_WEBFS_SERVE_CORE_H_ */
