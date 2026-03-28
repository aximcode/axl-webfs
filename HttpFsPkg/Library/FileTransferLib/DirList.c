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
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>HttpFS &mdash; Volumes</title>"
            "<style>"
            "body{background:#1a1a2e;color:#e0e0e0;font-family:-apple-system,"
            "BlinkMacSystemFont,\"Segoe UI\",Roboto,monospace;margin:0;padding:2em}"
            "a{color:#4a9eff;text-decoration:none}a:hover{color:#6ab4ff}"
            "h2{color:#4a9eff;margin-bottom:.3em}"
            ".crumb{color:#8888aa;margin-bottom:1.5em;font-size:.9em}"
            ".card{background:#16213e;border:1px solid #0f3460;border-radius:8px;"
            "padding:1.5em;max-width:900px}"
            "table{border-collapse:collapse;width:100%%}"
            "th{text-align:left;padding:8px 12px;border-bottom:2px solid #0f3460;"
            "color:#8888aa;font-size:.85em;text-transform:uppercase}"
            "td{padding:8px 12px;border-bottom:1px solid #0f3460}"
            "tr:hover{background:#1a4a80}"
            "</style></head><body>"
            "<h2>HttpFS</h2>"
            "<div class=\"crumb\">Volumes</div>"
            "<div class=\"card\"><table>"
            "<tr><th>Volume</th></tr>");

        for (UINTN i = 0; i < Count; i++) {
            FT_VOLUME Vol;
            FileTransferGetVolume(i, &Vol);
            Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                "<tr><td><a href=\"/%s/\">%s:</a></td></tr>", Vol.Name, Vol.Name);
        }

        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "</table></div></body></html>");
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
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>%s:%s</title>"
            "<style>"
            "body{background:#1a1a2e;color:#e0e0e0;font-family:-apple-system,"
            "BlinkMacSystemFont,\"Segoe UI\",Roboto,monospace;margin:0;padding:2em}"
            "a{color:#4a9eff;text-decoration:none}a:hover{color:#6ab4ff}"
            "h2{color:#4a9eff;margin-bottom:.3em}"
            ".crumb{color:#8888aa;margin-bottom:1.5em;font-size:.9em}"
            ".crumb a{color:#4a9eff}"
            ".card{background:#16213e;border:1px solid #0f3460;border-radius:8px;"
            "padding:1.5em;max-width:900px}"
            "table{border-collapse:collapse;width:100%%}"
            "th{text-align:left;padding:8px 12px;border-bottom:2px solid #0f3460;"
            "color:#8888aa;font-size:.85em;text-transform:uppercase}"
            "td{padding:8px 12px;border-bottom:1px solid #0f3460}"
            "tr:hover{background:#1a4a80}"
            ".sz{text-align:right;color:#8888aa;white-space:nowrap}"
            ".dt{color:#8888aa;white-space:nowrap}.ty{color:#8888aa}"
            "</style></head><body>"
            "<h2>HttpFS</h2>",
            Volume->Name, Path);

        // Breadcrumb: Volumes / fs0: / path / subdir
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "<div class=\"crumb\"><a href=\"/\">Volumes</a>"
            " / <a href=\"/%s/\">%s:</a>", Volume->Name, Volume->Name);

        // Add path segments as breadcrumb links
        {
            CHAR8 Href[512];
            UINTN HrefLen = AsciiSPrint(Href, sizeof(Href), "/%s", Volume->Name);
            CHAR8 SegBuf[256];
            UINTN si = 0;
            UINTN pi = 0;
            if (Path[0] == L'/') pi = 1;

            while (Path[pi] != L'\0') {
                if (Path[pi] == L'/') {
                    SegBuf[si] = '\0';
                    if (si > 0) {
                        HrefLen += AsciiSPrint(Href + HrefLen,
                            sizeof(Href) - HrefLen, "/%a", SegBuf);
                        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                            " / <a href=\"%a/\">%a</a>", Href, SegBuf);
                    }
                    si = 0;
                } else {
                    if (si < 255) SegBuf[si++] = (CHAR8)Path[pi];
                }
                pi++;
            }
            // Last segment (current directory — not a link)
            if (si > 0) {
                SegBuf[si] = '\0';
                Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                    " / %a", SegBuf);
            }
        }

        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, "</div>");

        // Determine parent link
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "<div class=\"card\"><table>"
            "<tr><th>Name</th><th class=\"sz\">Size</th><th>Modified</th><th>Type</th></tr>");

        // ".." parent directory link
        {
            // Find parent path: strip last segment from Path
            CHAR8 ParentHref[512];
            UINTN pi = 0;
            while (Path[pi] != L'\0' && pi < 511) {
                ParentHref[pi] = (CHAR8)Path[pi];
                pi++;
            }
            ParentHref[pi] = '\0';
            // Remove trailing slash
            if (pi > 0 && ParentHref[pi - 1] == '/') ParentHref[--pi] = '\0';
            // Find last slash
            INTN Last = (INTN)pi - 1;
            while (Last >= 0 && ParentHref[Last] != '/') Last--;
            if (Last >= 0) {
                ParentHref[Last + 1] = '\0';
                Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                    "<tr><td><a href=\"/%s%a\">..</a></td>"
                    "<td class=\"sz\">&mdash;</td><td class=\"dt\"></td><td class=\"ty\">dir</td></tr>",
                    Volume->Name, ParentHref);
            } else {
                // At volume root — parent is volume list
                Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                    "<tr><td><a href=\"/\">..</a></td>"
                    "<td class=\"sz\">&mdash;</td><td class=\"dt\"></td><td class=\"ty\">dir</td></tr>");
            }
        }
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
            EFI_TIME *Mt = &Info->ModificationTime;
            if (IsDir) {
                Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                    "<tr><td><a href=\"%s/\">%s/</a></td>"
                    "<td class=\"sz\">&mdash;</td>"
                    "<td class=\"dt\">%04d-%02d-%02d %02d:%02d</td>"
                    "<td class=\"ty\">dir</td></tr>",
                    Info->FileName, Info->FileName,
                    Mt->Year, Mt->Month, Mt->Day, Mt->Hour, Mt->Minute);
            } else {
                Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
                    "<tr><td><a href=\"%s\">%s</a></td>"
                    "<td class=\"sz\">%llu</td>"
                    "<td class=\"dt\">%04d-%02d-%02d %02d:%02d</td>"
                    "<td class=\"ty\">file</td></tr>",
                    Info->FileName, Info->FileName, Info->FileSize,
                    Mt->Year, Mt->Month, Mt->Day, Mt->Hour, Mt->Minute);
            }
        }

        if (Pos >= BufferSize - 256) break;  // Safety margin
    }

    if (AsJson) {
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos, "]");
    } else {
        Pos += AsciiSPrint(Buffer + Pos, BufferSize - Pos,
            "</table></div></body></html>");
    }

    Dir->Close(Dir);
    *Written = Pos;
    return EFI_SUCCESS;
}
