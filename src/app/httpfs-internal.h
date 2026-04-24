/** @file
  HttpFS -- Internal declarations shared between Main.c and command files.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef HTTPFS_INTERNAL_H_
#define HTTPFS_INTERNAL_H_

#include <axl.h>

int cmd_mount(int argc, char **argv);
int cmd_umount(int argc, char **argv);
int cmd_serve(int argc, char **argv);

#endif // HTTPFS_INTERNAL_H_
