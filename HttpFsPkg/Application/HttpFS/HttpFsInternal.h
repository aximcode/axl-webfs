/** @file
  HttpFS — Internal declarations shared between Main.c and command files.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HTTPFS_INTERNAL_H_
#define HTTPFS_INTERNAL_H_

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS CmdMount (IN EFI_HANDLE ImageHandle, IN UINTN Argc, IN CHAR16 **Argv);
EFI_STATUS CmdUmount (IN EFI_HANDLE ImageHandle, IN UINTN Argc, IN CHAR16 **Argv);
EFI_STATUS CmdServe (IN EFI_HANDLE ImageHandle, IN UINTN Argc, IN CHAR16 **Argv);

#endif // HTTPFS_INTERNAL_H_
