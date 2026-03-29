## @file
#  HttpFS — UEFI File Transfer Toolkit
#
#  Copyright (c) 2026, AximCode. All rights reserved.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME           = HttpFsPkg
  PLATFORM_GUID           = 5C6D7E8F-0A1B-4C3D-E4F5-061728394A5B
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/HttpFsPkg
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

  # Shell/HII support (needed by AxlLib)
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  TimerLib|MdePkg/Library/BaseTimerLibNullTemplate/BaseTimerLibNullTemplate.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  NULL|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  StackCheckFailureHookLib|MdePkg/Library/StackCheckFailureHookLibNull/StackCheckFailureHookLibNull.inf

  # AxlLib (from libaxl via PACKAGES_PATH)
  AxlMemLib|AxlPkg/Lib/Mem/AxlMem.inf
  AxlFormatLib|AxlPkg/Lib/Format/AxlFormat.inf
  AxlIOLib|AxlPkg/Lib/IO/AxlIO.inf
  AxlAppLib|AxlPkg/Lib/Posix/AxlApp.inf
  AxlLogLib|AxlPkg/Lib/Log/AxlLog.inf
  AxlDataLib|AxlPkg/Lib/Data/AxlData.inf
  AxlUtilLib|AxlPkg/Lib/Util/AxlUtil.inf
  AxlLoopLib|AxlPkg/Lib/Loop/AxlLoop.inf
  AxlTaskLib|AxlPkg/Lib/Task/AxlTask.inf
  AxlNetLib|AxlPkg/Lib/Net/AxlNet.inf

  # HttpFsPkg libraries
  NetworkLib|HttpFsPkg/Library/NetworkLib/NetworkLib.inf
  FileTransferLib|HttpFsPkg/Library/FileTransferLib/FileTransferLib.inf
  JsonLib|HttpFsPkg/Library/JsonLib/JsonLib.inf

[Components]
  HttpFsPkg/Application/HttpFS/HttpFS.inf
  HttpFsPkg/Driver/WebDavFsDxe/WebDavFsDxe.inf

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -Wno-unused-variable -Wno-unused-function
  # UEFI firmware does not enable AVX in CR4 — prevent GCC auto-vectorization
  GCC:*_*_X64_CC_FLAGS = -mno-avx -mno-avx2
