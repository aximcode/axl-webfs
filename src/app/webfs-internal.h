/** @file
  axl-webfs -- Internal declarations shared between Main.c and command files.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_INTERNAL_H_
#define AXL_WEBFS_INTERNAL_H_

#include <axl.h>
#include <stddef.h>

#define AXL_WEBFS_VERSION  "0.2"

/* Embedded upload.js asset (defined in upload-asset.c). */
extern const char   upload_js[];
extern const size_t upload_js_len;

#endif // AXL_WEBFS_INTERNAL_H_
