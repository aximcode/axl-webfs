/** @file
  axl-webfs -- WebDAV class-1 + MOVE adapter: volumes as a virtual root.

  Serves the SDK's filesystem WebDAV backend (axl_fs_webdav_ops) per
  volume, under a virtual root of volumes; MOVE and Want-Digest map
  onto file-transfer.{c,h}.
  Registers /dav as the WebDAV root; clients see one virtual
  collection per UEFI volume directly under it (e.g. /dav/fs0/).

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_DAV_H_
#define AXL_WEBFS_DAV_H_

#include <axl.h>

#include "transfer/file-transfer.h"

/// Register the WebDAV mount on @p server at @p prefix. @p auth_flags
/// (AXL_ROUTE_* — NO_AUTH/AUTH/ADMIN) gates every verb route of the
/// mount through the server's auth callback. Returns AXL_OK on success
/// or the underlying axl_http_server_add_webdav_auth status. The mount
/// stays valid until @p server is freed. Call after ft_init: it makes
/// one SDK filesystem root per volume found then.
int
webfs_dav_register(AxlHttpServer *server, const char *prefix,
                   uint32_t auth_flags);

/// Atomic PUT of volume-relative @p sub ("/a/b.txt") on @p vol, for the
/// REST surface as much as /dav: the SDK's axl_fs_webdav_ops() write path
/// (temp sibling, axl_file_replace on commit, discard through the
/// writer's own handle). Returns AXL_OK with @p out_ctx set, or the
/// SDK's refusal: AXL_IS_DIR (a collection there), AXL_EXISTS,
/// AXL_DENIED (read-only, or a live PUT temp), AXL_NO_RESOURCES,
/// AXL_BUSY, AXL_ERR (no parent directory, I/O). Needs
/// webfs_dav_register first.
int
webfs_put_open(const FtVolume *vol, const char *sub, void **out_ctx);

/// Append @p len bytes to the PUT's temp. AXL_OK, or a negative status.
int
webfs_put_write(void *ctx, const void *data, size_t len);

/// End the PUT exactly once: @p discard false commits (flush + replace;
/// the status is the PUT's, as webfs_put_open lists, and the path's
/// cached digest is dropped), true discards (the temp is deleted, the
/// target is untouched; the return means nothing). @p ctx is dead after.
int
webfs_put_close(void *ctx, bool discard);

/// Free what webfs_dav_register made. Call after freeing the server
/// that carries the mount. Safe without a prior register.
void
webfs_dav_unregister(void);

#endif /* AXL_WEBFS_DAV_H_ */
