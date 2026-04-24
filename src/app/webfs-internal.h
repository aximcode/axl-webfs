/** @file
  axl-webfs -- Internal declarations shared between Main.c and command files.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef AXL_WEBFS_INTERNAL_H_
#define AXL_WEBFS_INTERNAL_H_

#include <axl.h>

#define AXL_WEBFS_VERSION  "0.2"

int cmd_mount(int argc, char **argv);
int cmd_umount(int argc, char **argv);
int cmd_serve(int argc, char **argv);

#endif // AXL_WEBFS_INTERNAL_H_
