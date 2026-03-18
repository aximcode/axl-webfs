/** @file
  FileTransferLib — Directory listing as JSON and HTML.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/FileTransferLib.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

// ----------------------------------------------------------------------------
// Volume listing
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI FileTransferListVolumes(
    IN  BOOLEAN  AsJson,
    OUT CHAR8    *Buffer,
    IN  UINTN   BufferSize,
    OUT UINTN   *Written
) {
    UINTN Pos = 0;
    UINTN Count = FileTransferGetVolumeCount();

    if (AsJson) {
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, "[");
        for (UINTN i = 0; i < Count; i++) {
            FT_VOLUME Vol;
            FileTransferGetVolume(i, &Vol);
            if (i > 0) Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, ",");
            Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                "{\"name\":\"%s\",\"index\":%d}", Vol.Name, i);
        }
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, "]");
    } else {
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "<html><head><title>UefiXfer — Volumes</title>"
            "<style>body{font-family:monospace;margin:2em}"
            "a{text-decoration:none}table{border-collapse:collapse}"
            "td,th{padding:4px 12px;text-align:left}"
            "</style></head><body>"
            "<h2>UefiXfer — UEFI Volumes</h2><table>"
            "<tr><th>Volume</th></tr>");

        for (UINTN i = 0; i < Count; i++) {
            FT_VOLUME Vol;
            FileTransferGetVolume(i, &Vol);
            Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                "<tr><td><a href=\"/%s/\">%s:</a></td></tr>", Vol.Name, Vol.Name);
        }

        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "</table></body></html>");
    }

    *Written = Pos;
    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// Directory listing
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI FileTransferListDir(
    IN  FT_VOLUME    *Volume,
    IN  CONST CHAR16 *Path,
    IN  BOOLEAN      AsJson,
    OUT CHAR8        *Buffer,
    IN  UINTN        BufferSize,
    OUT UINTN        *Written
) {
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_STATUS Status = Volume->Fs->OpenVolume(Volume->Fs, &Root);
    if (EFI_ERROR(Status)) return Status;

    // Convert path separators
    CHAR16 UefiPath[512];
    UINTN i = 0;
    while (Path[i] != L'\0' && i < 511) {
        UefiPath[i] = (Path[i] == L'/') ? L'\\' : Path[i];
        i++;
    }
    UefiPath[i] = L'\0';

    CHAR16 *OpenPath = UefiPath;
    if (OpenPath[0] == L'\\') OpenPath++;

    EFI_FILE_PROTOCOL *Dir = Root;
    if (OpenPath[0] != L'\0') {
        Status = Root->Open(Root, &Dir, OpenPath, EFI_FILE_MODE_READ, 0);
        Root->Close(Root);
        if (EFI_ERROR(Status)) return Status;
    }

    UINTN Pos = 0;
    BOOLEAN First = TRUE;

    if (AsJson) {
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, "[");
    } else {
        // Build path string for title
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "<html><head><title>%s:%s</title>"
            "<style>body{font-family:monospace;margin:2em}"
            "a{text-decoration:none}table{border-collapse:collapse}"
            "td,th{padding:4px 12px;text-align:left}"
            "</style></head><body>"
            "<h2>%s:%s</h2><table>"
            "<tr><th>Name</th><th>Size</th><th>Type</th></tr>",
            Volume->Name, Path, Volume->Name, Path);
    }

    // Read directory entries
    UINT8 InfoBuf[512];
    for (;;) {
        UINTN InfoSize = sizeof(InfoBuf);
        Status = Dir->Read(Dir, &InfoSize, InfoBuf);
        if (EFI_ERROR(Status) || InfoSize == 0) break;

        EFI_FILE_INFO *Info = (EFI_FILE_INFO *)InfoBuf;
        BOOLEAN IsDir = (Info->Attribute & EFI_FILE_DIRECTORY) != 0;

        // Skip . and ..
        if (StrCmp(Info->FileName, L".") == 0 || StrCmp(Info->FileName, L"..") == 0) {
            continue;
        }

        if (AsJson) {
            if (!First) Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, ",");
            First = FALSE;
            Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                "{\"name\":\"%s\",\"size\":%llu,\"dir\":%a}",
                Info->FileName, Info->FileSize,
                IsDir ? "true" : "false");
        } else {
            if (IsDir) {
                Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                    "<tr><td><a href=\"%s/\">%s/</a></td><td>—</td><td>dir</td></tr>",
                    Info->FileName, Info->FileName);
            } else {
                Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                    "<tr><td><a href=\"%s\">%s</a></td><td>%llu</td><td>file</td></tr>",
                    Info->FileName, Info->FileName, Info->FileSize);
            }
        }

        if (Pos >= BufferSize - 256) break;  // Safety margin
    }

    if (AsJson) {
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, "]");
    } else {
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "</table></body></html>");
    }

    Dir->Close(Dir);
    *Written = Pos;
    return EFI_SUCCESS;
}
