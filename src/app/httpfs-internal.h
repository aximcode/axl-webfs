/** @file
  HttpFS -- Internal declarations shared between Main.c and command files.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HTTPFS_INTERNAL_H_
#define HTTPFS_INTERNAL_H_

#include <axl.h>

int CmdMount(int argc, char **argv);
int CmdUmount(int argc, char **argv);
int CmdServe(int argc, char **argv);

#endif // HTTPFS_INTERNAL_H_
