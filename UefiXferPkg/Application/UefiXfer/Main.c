/** @file
  UefiXfer — UEFI File Transfer Toolkit entry point.

  Parses Shell command line parameters and dispatches to the
  appropriate command handler: serve, mount, or umount.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiXferInternal.h"

#include <Library/BaseLib.h>
#include <Protocol/ShellParameters.h>

#define UEFIXFER_VERSION  L"0.1"

static VOID PrintUsage(VOID) {
    Print(L"UefiXfer v%s — UEFI File Transfer Toolkit\n\n", UEFIXFER_VERSION);
    Print(L"Usage:\n");
    Print(L"  UefiXfer mount <url> [-r]   Mount remote directory as UEFI volume\n");
    Print(L"  UefiXfer umount             Unmount remote volume\n");
    Print(L"  UefiXfer serve [options]    Run HTTP file server (Phase 3)\n");
    Print(L"  UefiXfer -h                 Show this help\n");
    Print(L"\nExamples:\n");
    Print(L"  UefiXfer mount http://10.0.0.5:8080/\n");
    Print(L"  UefiXfer umount\n");
}

EFI_STATUS
EFIAPI
UefiXferMain (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable
    )
{
    // Get Shell parameters for argc/argv
    EFI_SHELL_PARAMETERS_PROTOCOL *ShellParams = NULL;
    EFI_STATUS Status = gBS->OpenProtocol(
        ImageHandle, &gEfiShellParametersProtocolGuid,
        (VOID **)&ShellParams, ImageHandle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (EFI_ERROR(Status) || ShellParams == NULL) {
        Print(L"UefiXfer v%s — requires UEFI Shell\n", UEFIXFER_VERSION);
        return EFI_UNSUPPORTED;
    }

    UINTN Argc = ShellParams->Argc;
    CHAR16 **Argv = ShellParams->Argv;

    if (Argc < 2) {
        PrintUsage();
        return EFI_SUCCESS;
    }

    CHAR16 *Cmd = Argv[1];

    if (StrCmp(Cmd, L"-h") == 0 || StrCmp(Cmd, L"--help") == 0 ||
        StrCmp(Cmd, L"help") == 0) {
        PrintUsage();
        return EFI_SUCCESS;
    }

    if (StrCmp(Cmd, L"mount") == 0) {
        return CmdMount(ImageHandle, Argc - 2, &Argv[2]);
    }

    if (StrCmp(Cmd, L"umount") == 0) {
        return CmdUmount(ImageHandle, Argc - 2, &Argv[2]);
    }

    if (StrCmp(Cmd, L"serve") == 0) {
        return CmdServe(ImageHandle, Argc - 2, &Argv[2]);
    }

    Print(L"ERROR: Unknown command '%s'\n\n", Cmd);
    PrintUsage();
    return EFI_INVALID_PARAMETER;
}
