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

/// Register the WebDAV mount on @p server at @p prefix. @p auth_flags
/// (AXL_ROUTE_* — NO_AUTH/AUTH/ADMIN) gates every verb route of the
/// mount through the server's auth callback. Returns AXL_OK on success
/// or the underlying axl_http_server_add_webdav_auth status. The mount
/// stays valid until @p server is freed. Call after ft_init: it makes
/// one SDK filesystem root per volume found then.
int
webfs_dav_register(AxlHttpServer *server, const char *prefix,
                   uint32_t auth_flags);

/// Free what webfs_dav_register made. Call after freeing the server
/// that carries the mount. Safe without a prior register.
void
webfs_dav_unregister(void);

#endif /* AXL_WEBFS_DAV_H_ */
