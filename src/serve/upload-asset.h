/** @file
  axl-webfs -- embedded upload UI script.

  Defined in upload-asset.c, served at /_axl-webfs/upload.js by the
  HTTP server (see serve-core.c handle_get_upload_js).

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_UPLOAD_ASSET_H_
#define AXL_WEBFS_UPLOAD_ASSET_H_

#include <stddef.h>

extern const char   upload_js[];
extern const size_t upload_js_len;

#endif /* AXL_WEBFS_UPLOAD_ASSET_H_ */
