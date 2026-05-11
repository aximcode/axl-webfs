/** @file
  axl-webfs -- mount service descriptor (linked into both binaries).

  webfs-mount.c defines MountOpts + the AxlService descriptor stub
  unconditionally; impl (setup, teardown, parse_url, EFI_FILE_PROTOCOL
  install) is gated on AXL_SERVICE_BUILD_DRIVER. The launcher only
  reads url + read_only out of MountOpts to serialize via opts_descs;
  the WebFsPrivate runtime context lives behind an opaque pointer
  that's set by setup() and only meaningful in the driver build.

  Cross-binary ABI rule: same source, same flags except for the
  AXL_SERVICE_BUILD_DRIVER toggle.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_MOUNT_H_
#define AXL_WEBFS_MOUNT_H_

#include <axl.h>
#include <axl/axl-net-opts.h>
#include <stdbool.h>
#include <stddef.h>

/// Configuration + (driver-side) runtime state for the mount service.
///
/// The auto-applied fields (@ref url, @ref read_only) are written by
/// AxlConfig auto-apply: launcher-side from the parsed `mount` verb,
/// driver-side from LoadOptions. STRING values land as pointers into
/// caller-owned storage per AxlConfig's contract.
///
/// `priv` is a runtime field set by mount_setup; it's compiled out of
/// the launcher's MountOpts so the launcher never references the
/// driver-only WebFsPrivate type. axl-sdk's serialization is offset-
/// driven against opts_descs, which doesn't list `priv`, so the launcher's
/// shorter MountOpts is wire-compatible with the driver's longer one.
typedef struct {
    /* Auto-applied from mount_descs */
    const char *url;
    bool        read_only;
    /// "auto" (probe), "json" (force JSON), or "dav" (force WebDAV).
    /// Probe issues OPTIONS / and picks DAV when the server returns
    /// a DAV: header, otherwise JSON.
    const char *protocol;
    /// Canonical AXL networking opts (NIC index + outbound source
    /// IP). mount uses AXL_NET_OPT_CLIENT — `net.nic_index` selects
    /// which NIC to DHCP on; `net.local_ip` pins the local end of
    /// the outbound socket via the HTTP client's `source.ip`. v1 of
    /// this struct carried a `static_ip` field with a comment
    /// claiming "mount has no listen socket" — the comment was
    /// wrong: the outbound bind IS the analogous knob, and it's
    /// what `--source-ip` configures now.
    AxlNetOpts  net;
    /// HTTP authentication spec. Empty / NULL disables auth.
    /// Formats:
    ///   "basic:<user>:<password-or-token>" → Authorization: Basic <b64>
    ///   "bearer:<token>"                   → Authorization: Bearer <token>
    /// Visible in UEFI Shell `history`; for token-typed protocols
    /// (Jenkins, GitHub, NextCloud personal-access-tokens) the
    /// "leak" is bounded to that token's per-user scope.
    const char *auth;

#ifdef AXL_SERVICE_BUILD_DRIVER
    /* Driver-side runtime — owned by setup, freed by teardown. */
    struct WebFsPrivate *priv;
#endif
} MountOpts;

/* Defined in webfs-mount.c, linked into both binaries. */
extern MountOpts          g_mount_opts;
extern const AxlConfigDesc mount_descs[];
extern const AxlService    webfs_mount;

#endif /* AXL_WEBFS_MOUNT_H_ */
