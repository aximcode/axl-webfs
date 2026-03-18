## @file
#  UefiXfer — UEFI File Transfer Toolkit
#
#  Copyright (c) 2026, AximCode. All rights reserved.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME           = UefiXferPkg
  PLATFORM_GUID           = 5C6D7E8F-0A1B-4C3D-E4F5-061728394A5B
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/UefiXferPkg
  SUPPORTED_ARCHITECTURES = X64|AARCH64
  BUILD_TARGETS           = DEBUG|RELEASE

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  # Standard MdePkg libraries
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

  # UefiXferPkg libraries
  NetworkLib|UefiXferPkg/Library/NetworkLib/NetworkLib.inf
  HttpClientLib|UefiXferPkg/Library/HttpClientLib/HttpClientLib.inf
  HttpServerLib|UefiXferPkg/Library/HttpServerLib/HttpServerLib.inf
  FileTransferLib|UefiXferPkg/Library/FileTransferLib/FileTransferLib.inf
  JsonLib|UefiXferPkg/Library/JsonLib/JsonLib.inf

[Components]
  UefiXferPkg/Application/UefiXfer/UefiXfer.inf
  UefiXferPkg/Driver/WebDavFsDxe/WebDavFsDxe.inf

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -Wno-unused-variable -Wno-unused-function
  GCC:*_*_X64_CC_FLAGS = -mno-avx
