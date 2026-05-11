/** @file
  axl-webfs -- WebDAV class-1 + MOVE adapter onto the ft_* helpers.

  Maps axl-sdk's AxlWebDavOps callbacks onto file-transfer.{c,h}.
  Registers /dav as the WebDAV root; clients see one virtual
  collection per UEFI volume directly under it (e.g. /dav/fs0/).

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_DAV_H_
#define AXL_WEBFS_DAV_H_

#include <axl.h>

/// Register the WebDAV mount on @p server at "/dav". Returns AXL_OK
/// on success or the underlying axl_http_server_add_webdav status.
/// The mount stays valid until @p server is freed.
int
webfs_dav_register(AxlHttpServer *server, const char *prefix);

#endif /* AXL_WEBFS_DAV_H_ */
