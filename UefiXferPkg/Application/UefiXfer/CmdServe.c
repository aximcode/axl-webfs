/** @file
  UefiXfer — serve command handler.

  Initializes the HTTP server, registers route handlers that map
  REST API endpoints to FileTransferLib operations, and runs the
  main poll loop with ESC detection and progress display.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiXferInternal.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/NetworkLib.h>
#include <Library/HttpServerLib.h>
#include <Library/FileTransferLib.h>

// ----------------------------------------------------------------------------
// Options
// ----------------------------------------------------------------------------

typedef struct {
    UINT16   Port;
    UINTN    NicIndex;
    UINTN    IdleTimeoutSec;
    BOOLEAN  ReadOnly;
    BOOLEAN  WriteOnly;
    BOOLEAN  Verbose;
} SERVE_OPTIONS;

// ----------------------------------------------------------------------------
// URL parsing helpers
// ----------------------------------------------------------------------------

/// Parse "/<vol>/<path>" into volume name and sub-path.
/// Returns the FT_VOLUME and sets *SubPath to the path after the volume name.
static EFI_STATUS ParseVolumePath(
    IN  CONST CHAR8   *UrlPath,
    OUT FT_VOLUME     *Volume,
    OUT CHAR16        *SubPath,
    IN  UINTN         SubPathSize
) {
    // Skip leading /
    CONST CHAR8 *P = UrlPath;
    if (*P == '/') P++;

    // Extract volume name (up to next / or end)
    CHAR16 VolName[16];
    UINTN i = 0;
    while (*P != '/' && *P != '\0' && i < 15) {
        VolName[i++] = (CHAR16)*P++;
    }
    VolName[i] = L'\0';

    EFI_STATUS Status = FileTransferFindVolume(VolName, Volume);
    if (EFI_ERROR(Status)) return Status;

    // Rest is the sub-path (convert ASCII to UCS-2)
    i = 0;
    while (*P != '\0' && i < SubPathSize - 1) {
        SubPath[i++] = (CHAR16)*P++;
    }
    SubPath[i] = L'\0';

    // Default to root if empty
    if (SubPath[0] == L'\0') {
        SubPath[0] = L'/';
        SubPath[1] = L'\0';
    }

    return EFI_SUCCESS;
}

/// Check if Accept header contains "application/json".
static BOOLEAN WantsJson(IN CONST CHAR8 *AcceptHeader) {
    if (AcceptHeader == NULL) return FALSE;
    // Simple substring check
    CONST CHAR8 *P = AcceptHeader;
    while (*P != '\0') {
        if (AsciiStrnCmp(P, "application/json", 16) == 0) return TRUE;
        P++;
    }
    return FALSE;
}

// ----------------------------------------------------------------------------
// Route handlers
// ----------------------------------------------------------------------------

/// GET / — list all volumes.
static EFI_STATUS HandleGetRoot(IN HTTP_SERVER_REQUEST *Request) {
    CHAR8 Buf[4096];
    UINTN Written = 0;
    BOOLEAN AsJson = WantsJson(Request->AcceptHeader);

    FileTransferListVolumes(AsJson, Buf, sizeof(Buf), &Written);

    CONST CHAR8 *ContentType = AsJson ? "application/json" : "text/html";
    return HttpServerSendResponse(200, ContentType, Buf, Written);
}

/// GET /<vol>/<path> — download file or list directory.
static EFI_STATUS HandleGetPath(IN HTTP_SERVER_REQUEST *Request) {
    FT_VOLUME Volume;
    CHAR16 SubPath[512];

    EFI_STATUS Status = ParseVolumePath(Request->Path, &Volume, SubPath, 512);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(404, "Volume not found");
        return Status;
    }

    // Check if directory
    BOOLEAN IsDir = FALSE;
    Status = FileTransferIsDir(&Volume, SubPath, &IsDir);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(404, "Not found");
        return Status;
    }

    if (IsDir) {
        CHAR8 Buf[8192];
        UINTN Written = 0;
        BOOLEAN AsJson = WantsJson(Request->AcceptHeader);

        Status = FileTransferListDir(&Volume, SubPath, AsJson, Buf, sizeof(Buf), &Written);
        if (EFI_ERROR(Status)) {
            HttpServerSendError(500, "Directory listing failed");
            return Status;
        }

        CONST CHAR8 *ContentType = AsJson ? "application/json" : "text/html";
        return HttpServerSendResponse(200, ContentType, Buf, Written);
    }

    // File download
    UINT64 FileSize = 0;
    Status = FileTransferGetFileSize(&Volume, SubPath, &FileSize);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(404, "File not found");
        return Status;
    }

    UINT64 Offset = 0;
    UINT16 StatusCode = 200;
    UINTN SendSize = (UINTN)FileSize;
    CHAR8 ExtraHeaders[128];
    ExtraHeaders[0] = '\0';

    // Handle Range header
    if (Request->RangeHeader != NULL) {
        // Parse "bytes=N-" or "bytes=N-M"
        CONST CHAR8 *R = Request->RangeHeader;
        if (AsciiStrnCmp(R, "bytes=", 6) == 0) {
            R += 6;
            UINT64 Start = 0;
            while (*R >= '0' && *R <= '9') { Start = Start * 10 + (*R - '0'); R++; }
            UINT64 End = FileSize - 1;
            if (*R == '-') {
                R++;
                if (*R >= '0' && *R <= '9') {
                    End = 0;
                    while (*R >= '0' && *R <= '9') { End = End * 10 + (*R - '0'); R++; }
                }
            }
            if (Start < FileSize) {
                Offset = Start;
                SendSize = (UINTN)(End - Start + 1);
                StatusCode = 206;
                AsciiSPrint(ExtraHeaders, sizeof(ExtraHeaders),
                    "Content-Range: bytes %llu-%llu/%llu\r\n", Start, End, FileSize);
            }
        }
    }

    Status = HttpServerSendHeaders(
        StatusCode, "application/octet-stream", SendSize,
        ExtraHeaders[0] != '\0' ? ExtraHeaders : NULL);
    if (EFI_ERROR(Status)) return Status;

    // Stream file content
    FT_READ_CTX ReadCtx;
    Status = FileTransferOpenRead(&Volume, SubPath, Offset, NULL, NULL, &ReadCtx);
    if (EFI_ERROR(Status)) return Status;

    UINT8 Chunk[FT_CHUNK_SIZE];
    UINTN TotalSent = 0;
    while (TotalSent < SendSize) {
        UINTN Want = sizeof(Chunk);
        if (TotalSent + Want > SendSize) Want = SendSize - TotalSent;
        UINTN Got = 0;
        Status = FileTransferReadChunk(&ReadCtx, Chunk, Want, &Got);
        if (EFI_ERROR(Status) || Got == 0) break;
        Status = HttpServerSendChunk(Chunk, Got);
        if (EFI_ERROR(Status)) break;
        TotalSent += Got;
    }

    FileTransferCloseRead(&ReadCtx);
    return EFI_SUCCESS;
}

/// PUT /<vol>/<path> — upload/overwrite file.
static EFI_STATUS HandlePutPath(IN HTTP_SERVER_REQUEST *Request) {
    FT_VOLUME Volume;
    CHAR16 SubPath[512];

    EFI_STATUS Status = ParseVolumePath(Request->Path, &Volume, SubPath, 512);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(404, "Volume not found");
        return Status;
    }

    FT_WRITE_CTX WriteCtx;
    Status = FileTransferOpenWrite(&Volume, SubPath, NULL, NULL, &WriteCtx);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(500, "Cannot create file");
        return Status;
    }

    // Stream upload body
    UINT8 Chunk[FT_CHUNK_SIZE];
    for (;;) {
        UINTN Got = 0;
        Status = HttpServerRecvBody(Chunk, sizeof(Chunk), &Got);
        if (EFI_ERROR(Status) || Got == 0) break;
        Status = FileTransferWriteChunk(&WriteCtx, Chunk, Got);
        if (EFI_ERROR(Status)) break;
    }

    FileTransferCloseWrite(&WriteCtx);
    return HttpServerSendResponse(201, "text/plain", "Created\n", 8);
}

/// DELETE /<vol>/<path> — delete file or empty directory.
static EFI_STATUS HandleDeletePath(IN HTTP_SERVER_REQUEST *Request) {
    FT_VOLUME Volume;
    CHAR16 SubPath[512];

    EFI_STATUS Status = ParseVolumePath(Request->Path, &Volume, SubPath, 512);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(404, "Volume not found");
        return Status;
    }

    Status = FileTransferDelete(&Volume, SubPath);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(404, "Not found");
        return Status;
    }

    return HttpServerSendResponse(200, "text/plain", "Deleted\n", 8);
}

/// POST /<vol>/<path>?mkdir — create directory.
static EFI_STATUS HandlePostPath(IN HTTP_SERVER_REQUEST *Request) {
    if (Request->QueryString == NULL ||
        AsciiStrCmp(Request->QueryString, "mkdir") != 0) {
        HttpServerSendError(400, "Use ?mkdir");
        return EFI_INVALID_PARAMETER;
    }

    FT_VOLUME Volume;
    CHAR16 SubPath[512];

    EFI_STATUS Status = ParseVolumePath(Request->Path, &Volume, SubPath, 512);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(404, "Volume not found");
        return Status;
    }

    Status = FileTransferMkdir(&Volume, SubPath);
    if (EFI_ERROR(Status)) {
        HttpServerSendError(500, "mkdir failed");
        return Status;
    }

    return HttpServerSendResponse(201, "text/plain", "Created\n", 8);
}

// ----------------------------------------------------------------------------
// Main serve command
// ----------------------------------------------------------------------------

EFI_STATUS CmdServe(
    IN EFI_HANDLE  ImageHandle,
    IN UINTN       Argc,
    IN CHAR16      **Argv
) {
    SERVE_OPTIONS Opts;
    Opts.Port = 8080;
    Opts.NicIndex = (UINTN)-1;
    Opts.IdleTimeoutSec = 0;
    Opts.ReadOnly = FALSE;
    Opts.WriteOnly = FALSE;
    Opts.Verbose = FALSE;

    // Parse options
    for (UINTN i = 0; i < Argc; i++) {
        if (StrCmp(Argv[i], L"-p") == 0 && i + 1 < Argc) {
            Opts.Port = (UINT16)StrDecimalToUintn(Argv[++i]);
        } else if (StrCmp(Argv[i], L"-n") == 0 && i + 1 < Argc) {
            Opts.NicIndex = StrDecimalToUintn(Argv[++i]);
        } else if (StrCmp(Argv[i], L"-t") == 0 && i + 1 < Argc) {
            Opts.IdleTimeoutSec = StrDecimalToUintn(Argv[++i]);
        } else if (StrCmp(Argv[i], L"--read-only") == 0) {
            Opts.ReadOnly = TRUE;
        } else if (StrCmp(Argv[i], L"--write-only") == 0) {
            Opts.WriteOnly = TRUE;
        } else if (StrCmp(Argv[i], L"-v") == 0) {
            Opts.Verbose = TRUE;
        } else if (StrCmp(Argv[i], L"-h") == 0) {
            Print(L"Usage: UefiXfer serve [options]\n");
            Print(L"  -p <port>       Listen port (default: 8080)\n");
            Print(L"  -n <index>      NIC index\n");
            Print(L"  -t <seconds>    Idle timeout\n");
            Print(L"  --read-only     Block uploads/deletes\n");
            Print(L"  --write-only    Block downloads\n");
            Print(L"  -v              Verbose logging\n");
            return EFI_SUCCESS;
        }
    }

    // Initialize networking
    EFI_STATUS Status = NetworkInit(ImageHandle, Opts.NicIndex, NULL, 10);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: Network init failed: %r\n", Status);
        return Status;
    }

    EFI_IPv4_ADDRESS Addr;
    NetworkGetAddress(&Addr);

    // Initialize file transfer
    Status = FileTransferInit();
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: FileTransfer init failed: %r\n", Status);
        NetworkCleanup();
        return Status;
    }

    // Initialize HTTP server
    EFI_HANDLE TcpSbHandle = NULL;
    Status = NetworkGetTcpServiceBinding(&TcpSbHandle);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: No TCP4 ServiceBinding: %r\n", Status);
        NetworkCleanup();
        return Status;
    }

    Status = HttpServerInit(ImageHandle, TcpSbHandle, Opts.Port);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: HTTP server init failed: %r\n", Status);
        NetworkCleanup();
        return Status;
    }

    // Set permissions
    HttpServerSetPermissions(Opts.ReadOnly, Opts.WriteOnly);

    // Register routes
    HttpServerRegisterRoute("GET", "/", HandleGetRoot,
        HTTP_ROUTE_FLAG_READ);
    HttpServerRegisterRoute("GET", "/", HandleGetPath,
        HTTP_ROUTE_FLAG_READ | HTTP_ROUTE_FLAG_PREFIX);
    HttpServerRegisterRoute("PUT", "/", HandlePutPath,
        HTTP_ROUTE_FLAG_WRITE | HTTP_ROUTE_FLAG_PREFIX);
    HttpServerRegisterRoute("DELETE", "/", HandleDeletePath,
        HTTP_ROUTE_FLAG_WRITE | HTTP_ROUTE_FLAG_PREFIX);
    HttpServerRegisterRoute("POST", "/", HandlePostPath,
        HTTP_ROUTE_FLAG_WRITE | HTTP_ROUTE_FLAG_PREFIX);

    // Print banner
    Print(L"\nUefiXfer v0.1 — UEFI HTTP File Server\n");
    Print(L"Listening on %d.%d.%d.%d:%d\n",
          Addr.Addr[0], Addr.Addr[1], Addr.Addr[2], Addr.Addr[3], Opts.Port);

    CONST CHAR16 *Mode = L"read-write";
    if (Opts.ReadOnly) Mode = L"read-only";
    if (Opts.WriteOnly) Mode = L"write-only";
    Print(L"Mode: %s\n", Mode);

    Print(L"Volumes:\n");
    UINTN VCount = FileTransferGetVolumeCount();
    for (UINTN i = 0; i < VCount; i++) {
        FT_VOLUME Vol;
        FileTransferGetVolume(i, &Vol);
        Print(L"  %s:\n", Vol.Name);
    }
    Print(L"Press ESC to stop.\n\n");

    // Main poll loop
    while (TRUE) {
        HttpServerPoll();

        // Check for ESC
        EFI_INPUT_KEY Key;
        EFI_STATUS KbStatus = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
        if (!EFI_ERROR(KbStatus) && Key.ScanCode == SCAN_ESC) {
            Print(L"\nESC — stopping server.\n");
            break;
        }

        gBS->Stall(1000);  // 1ms to avoid busy-spin
    }

    HttpServerStop();
    NetworkCleanup();
    return EFI_SUCCESS;
}
