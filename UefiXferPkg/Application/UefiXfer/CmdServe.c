/** @file
  UefiXfer — serve command handler.

  HTTP file server using UdkNet's UdkHttpServer. Registers route
  handlers that map REST API endpoints to FileTransferLib operations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiXferInternal.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/NetworkLib.h>
#include <Library/FileTransferLib.h>
#include <Library/UdkLib.h>
#include <Library/UdkNet.h>

UDK_LOG_DOMAIN ("serve");

// ----------------------------------------------------------------------------
// Options (passed as handler Data via route registration)
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
STATIC EFI_STATUS
ParseVolumePath (
  IN  CONST CHAR8  *UrlPath,
  OUT FT_VOLUME    *Volume,
  OUT CHAR16       *SubPath,
  IN  UINTN        SubPathSize
  )
{
  CONST CHAR8  *P = UrlPath;
  CHAR16       VolName[16];
  UINTN        I;

  if (*P == '/') {
    P++;
  }

  I = 0;
  while (*P != '/' && *P != '\0' && I < 15) {
    VolName[I++] = (CHAR16)*P++;
  }

  VolName[I] = L'\0';

  EFI_STATUS  Status = FileTransferFindVolume (VolName, Volume);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  I = 0;
  while (*P != '\0' && I < SubPathSize - 1) {
    SubPath[I++] = (CHAR16)*P++;
  }

  SubPath[I] = L'\0';

  if (SubPath[0] == L'\0') {
    SubPath[0] = L'/';
    SubPath[1] = L'\0';
  }

  return EFI_SUCCESS;
}

/// Check if Accept header requests JSON.
STATIC BOOLEAN
WantsJson (
  IN UDK_HTTP_REQUEST  *Req
  )
{
  CONST CHAR8  *Accept = (CONST CHAR8 *)UdkHashGet (Req->Headers, "accept");

  if (Accept == NULL) {
    return FALSE;
  }

  CONST CHAR8  *P = Accept;
  while (*P != '\0') {
    if (AsciiStrnCmp (P, "application/json", 16) == 0) {
      return TRUE;
    }

    P++;
  }

  return FALSE;
}

// ----------------------------------------------------------------------------
// Permission middleware
// ----------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
PermissionMiddleware (
  UDK_HTTP_REQUEST   *Req,
  UDK_HTTP_RESPONSE  *Resp,
  VOID               *Data
  )
{
  SERVE_OPTIONS  *Opts = (SERVE_OPTIONS *)Data;

  if (Opts->ReadOnly &&
      (AsciiStrCmp (Req->Method, "PUT") == 0 ||
       AsciiStrCmp (Req->Method, "DELETE") == 0 ||
       AsciiStrCmp (Req->Method, "POST") == 0))
  {
    UdkHttpResponseSetText (Resp, "Read-only mode\n");
    UdkHttpResponseSetStatus (Resp, 403);
    return EFI_ACCESS_DENIED;
  }

  if (Opts->WriteOnly && AsciiStrCmp (Req->Method, "GET") == 0) {
    UdkHttpResponseSetText (Resp, "Write-only mode\n");
    UdkHttpResponseSetStatus (Resp, 403);
    return EFI_ACCESS_DENIED;
  }

  return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// Route handlers
// ----------------------------------------------------------------------------

/// GET / — list all volumes.
STATIC
EFI_STATUS
EFIAPI
HandleGetRoot (
  UDK_HTTP_REQUEST   *Req,
  UDK_HTTP_RESPONSE  *Resp,
  VOID               *Data
  )
{
  CHAR8    Buf[4096];
  UINTN    Written = 0;
  BOOLEAN  AsJson = WantsJson (Req);

  (VOID)Data;

  FileTransferListVolumes (AsJson, Buf, sizeof (Buf), &Written);

  if (AsJson) {
    UdkHttpResponseSetJson (Resp, Buf);
  } else {
    Resp->Body = AllocatePool (Written);
    if (Resp->Body != NULL) {
      CopyMem (Resp->Body, Buf, Written);
      Resp->BodySize = Written;
    }

    Resp->ContentType = "text/html";
    Resp->StatusCode = 200;
  }

  return EFI_SUCCESS;
}

/// GET /<vol>/<path> — download file or list directory.
STATIC
EFI_STATUS
EFIAPI
HandleGetPath (
  UDK_HTTP_REQUEST   *Req,
  UDK_HTTP_RESPONSE  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;
  BOOLEAN     IsDir;

  (VOID)Data;

  Status = ParseVolumePath (Req->Path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Volume not found\n");
    UdkHttpResponseSetStatus (Resp, 404);
    return EFI_SUCCESS;
  }

  IsDir = FALSE;
  Status = FileTransferIsDir (&Volume, SubPath, &IsDir);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Not found\n");
    UdkHttpResponseSetStatus (Resp, 404);
    return EFI_SUCCESS;
  }

  if (IsDir) {
    CHAR8    Buf[8192];
    UINTN    Written = 0;
    BOOLEAN  AsJson = WantsJson (Req);

    Status = FileTransferListDir (&Volume, SubPath, AsJson, Buf, sizeof (Buf), &Written);
    if (EFI_ERROR (Status)) {
      UdkHttpResponseSetText (Resp, "Directory listing failed\n");
      UdkHttpResponseSetStatus (Resp, 500);
      return EFI_SUCCESS;
    }

    if (AsJson) {
      UdkHttpResponseSetJson (Resp, Buf);
    } else {
      Resp->Body = AllocatePool (Written);
      if (Resp->Body != NULL) {
        CopyMem (Resp->Body, Buf, Written);
        Resp->BodySize = Written;
      }

      Resp->ContentType = "text/html";
      Resp->StatusCode = 200;
    }

    return EFI_SUCCESS;
  }

  //
  // File download — read entire file into memory
  //
  UINT64  FileSize = 0;
  Status = FileTransferGetFileSize (&Volume, SubPath, &FileSize);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "File not found\n");
    UdkHttpResponseSetStatus (Resp, 404);
    return EFI_SUCCESS;
  }

  UINT64  Offset = 0;
  UINTN   SendSize = (UINTN)FileSize;

  //
  // Handle Range header
  //
  CONST CHAR8  *RangeHdr = (CONST CHAR8 *)UdkHashGet (Req->Headers, "range");
  if (RangeHdr != NULL && AsciiStrnCmp (RangeHdr, "bytes=", 6) == 0) {
    CONST CHAR8  *R = RangeHdr + 6;
    UINT64       Start = 0;
    UINT64       End = FileSize - 1;

    while (*R >= '0' && *R <= '9') {
      Start = Start * 10 + (*R - '0');
      R++;
    }

    if (*R == '-') {
      R++;
      if (*R >= '0' && *R <= '9') {
        End = 0;
        while (*R >= '0' && *R <= '9') {
          End = End * 10 + (*R - '0');
          R++;
        }
      }
    }

    if (Start < FileSize) {
      Offset = Start;
      SendSize = (UINTN)(End - Start + 1);
      Resp->StatusCode = 206;

      //
      // Set Content-Range header
      //
      if (Resp->Headers == NULL) {
        Resp->Headers = UdkHashNew ();
      }

      if (Resp->Headers != NULL) {
        CHAR8  RangeBuf[128];
        AsciiSPrint (RangeBuf, sizeof (RangeBuf),
          "bytes %Lu-%Lu/%Lu", (UINT64)Start, (UINT64)End, (UINT64)FileSize);
        UdkHashSet (Resp->Headers, "content-range", UdkStrDup (RangeBuf));
      }
    }
  }

  //
  // Read file content
  //
  FT_READ_CTX  ReadCtx;
  Status = FileTransferOpenRead (&Volume, SubPath, Offset, NULL, NULL, &ReadCtx);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Cannot open file\n");
    UdkHttpResponseSetStatus (Resp, 500);
    return EFI_SUCCESS;
  }

  VOID  *FileBuf = AllocatePool (SendSize);
  if (FileBuf == NULL) {
    FileTransferCloseRead (&ReadCtx);
    UdkHttpResponseSetText (Resp, "Out of memory\n");
    UdkHttpResponseSetStatus (Resp, 500);
    return EFI_SUCCESS;
  }

  UINTN  TotalRead = 0;
  while (TotalRead < SendSize) {
    UINTN  Got = 0;
    UINTN  Want = SendSize - TotalRead;
    if (Want > FT_CHUNK_SIZE) {
      Want = FT_CHUNK_SIZE;
    }

    Status = FileTransferReadChunk (&ReadCtx, (UINT8 *)FileBuf + TotalRead, Want, &Got);
    if (EFI_ERROR (Status) || Got == 0) {
      break;
    }

    TotalRead += Got;
  }

  FileTransferCloseRead (&ReadCtx);

  Resp->Body = FileBuf;
  Resp->BodySize = TotalRead;
  Resp->ContentType = "application/octet-stream";
  if (Resp->StatusCode == 0) {
    Resp->StatusCode = 200;
  }

  return EFI_SUCCESS;
}

/// PUT /<vol>/<path> — upload/overwrite file.
STATIC
EFI_STATUS
EFIAPI
HandlePutPath (
  UDK_HTTP_REQUEST   *Req,
  UDK_HTTP_RESPONSE  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;

  (VOID)Data;

  Status = ParseVolumePath (Req->Path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Volume not found\n");
    UdkHttpResponseSetStatus (Resp, 404);
    return EFI_SUCCESS;
  }

  FT_WRITE_CTX  WriteCtx;
  Status = FileTransferOpenWrite (&Volume, SubPath, NULL, NULL, &WriteCtx);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Cannot create file\n");
    UdkHttpResponseSetStatus (Resp, 500);
    return EFI_SUCCESS;
  }

  //
  // Write request body (already fully read by UdkHttpServer)
  //
  if (Req->Body != NULL && Req->BodySize > 0) {
    UINTN  Written = 0;
    while (Written < Req->BodySize) {
      UINTN  ChunkSize = Req->BodySize - Written;
      if (ChunkSize > FT_CHUNK_SIZE) {
        ChunkSize = FT_CHUNK_SIZE;
      }

      Status = FileTransferWriteChunk (&WriteCtx,
                 (CONST UINT8 *)Req->Body + Written, ChunkSize);
      if (EFI_ERROR (Status)) {
        break;
      }

      Written += ChunkSize;
    }
  }

  FileTransferCloseWrite (&WriteCtx);
  UdkHttpResponseSetText (Resp, "Created\n");
  UdkHttpResponseSetStatus (Resp, 201);
  return EFI_SUCCESS;
}

/// DELETE /<vol>/<path> — delete file or empty directory.
STATIC
EFI_STATUS
EFIAPI
HandleDeletePath (
  UDK_HTTP_REQUEST   *Req,
  UDK_HTTP_RESPONSE  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;

  (VOID)Data;

  Status = ParseVolumePath (Req->Path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Volume not found\n");
    UdkHttpResponseSetStatus (Resp, 404);
    return EFI_SUCCESS;
  }

  Status = FileTransferDelete (&Volume, SubPath);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Not found\n");
    UdkHttpResponseSetStatus (Resp, 404);
    return EFI_SUCCESS;
  }

  UdkHttpResponseSetText (Resp, "Deleted\n");
  return EFI_SUCCESS;
}

/// POST /<vol>/<path>?mkdir — create directory.
STATIC
EFI_STATUS
EFIAPI
HandlePostPath (
  UDK_HTTP_REQUEST   *Req,
  UDK_HTTP_RESPONSE  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;

  (VOID)Data;

  if (Req->Query == NULL || AsciiStrCmp (Req->Query, "mkdir") != 0) {
    UdkHttpResponseSetText (Resp, "Use ?mkdir\n");
    UdkHttpResponseSetStatus (Resp, 400);
    return EFI_SUCCESS;
  }

  Status = ParseVolumePath (Req->Path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "Volume not found\n");
    UdkHttpResponseSetStatus (Resp, 404);
    return EFI_SUCCESS;
  }

  Status = FileTransferMkdir (&Volume, SubPath);
  if (EFI_ERROR (Status)) {
    UdkHttpResponseSetText (Resp, "mkdir failed\n");
    UdkHttpResponseSetStatus (Resp, 500);
    return EFI_SUCCESS;
  }

  UdkHttpResponseSetText (Resp, "Created\n");
  UdkHttpResponseSetStatus (Resp, 201);
  return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// ESC key handler for the event loop
// ----------------------------------------------------------------------------

STATIC UDK_LOOP  *mServeLoop = NULL;

STATIC
BOOLEAN
EFIAPI
EscKeyHandler (
  IN EFI_INPUT_KEY  Key,
  IN VOID           *Data
  )
{
  (VOID)Data;

  if (Key.ScanCode == SCAN_ESC) {
    Print (L"\nESC \u2014 stopping server.\n");
    if (mServeLoop != NULL) {
      UdkLoopQuit (mServeLoop);
    }

    return FALSE;
  }

  return TRUE;
}

// ----------------------------------------------------------------------------
// Main serve command
// ----------------------------------------------------------------------------

EFI_STATUS
CmdServe (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       Argc,
  IN CHAR16      **Argv
  )
{
  SERVE_OPTIONS    Opts;
  EFI_STATUS       Status;
  UDK_HTTP_SERVER  *Server;

  Opts.Port = 8080;
  Opts.NicIndex = (UINTN)-1;
  Opts.IdleTimeoutSec = 0;
  Opts.ReadOnly = FALSE;
  Opts.WriteOnly = FALSE;
  Opts.Verbose = FALSE;

  // Parse options
  for (UINTN I = 0; I < Argc; I++) {
    if (StrCmp (Argv[I], L"-p") == 0 && I + 1 < Argc) {
      Opts.Port = (UINT16)StrDecimalToUintn (Argv[++I]);
    } else if (StrCmp (Argv[I], L"-n") == 0 && I + 1 < Argc) {
      Opts.NicIndex = StrDecimalToUintn (Argv[++I]);
    } else if (StrCmp (Argv[I], L"-t") == 0 && I + 1 < Argc) {
      Opts.IdleTimeoutSec = StrDecimalToUintn (Argv[++I]);
    } else if (StrCmp (Argv[I], L"--read-only") == 0) {
      Opts.ReadOnly = TRUE;
    } else if (StrCmp (Argv[I], L"--write-only") == 0) {
      Opts.WriteOnly = TRUE;
    } else if (StrCmp (Argv[I], L"-v") == 0) {
      Opts.Verbose = TRUE;
    } else if (StrCmp (Argv[I], L"-h") == 0) {
      Print (L"Usage: UefiXfer serve [options]\n");
      Print (L"  -p <port>       Listen port (default: 8080)\n");
      Print (L"  -n <index>      NIC index\n");
      Print (L"  -t <seconds>    Idle timeout\n");
      Print (L"  --read-only     Block uploads/deletes\n");
      Print (L"  --write-only    Block downloads\n");
      Print (L"  -v              Verbose logging\n");
      return EFI_SUCCESS;
    }
  }

  //
  // Initialize networking (driver loading, DHCP)
  //
  Status = NetworkInit (ImageHandle, Opts.NicIndex, NULL, 10);
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: Network init failed: %r\n", Status);
    return Status;
  }

  EFI_IPv4_ADDRESS  Addr;
  NetworkGetAddress (&Addr);

  //
  // Initialize file transfer
  //
  Status = FileTransferInit ();
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: FileTransfer init failed: %r\n", Status);
    NetworkCleanup ();
    return Status;
  }

  //
  // Create HTTP server
  //
  Server = UdkHttpServerNew (Opts.Port);
  if (Server == NULL) {
    Print (L"ERROR: HTTP server creation failed\n");
    NetworkCleanup ();
    return EFI_OUT_OF_RESOURCES;
  }

  UdkHttpServerSetBodyLimit (Server, 128 * 1024 * 1024);  // 128 MB

  //
  // Permission middleware
  //
  if (Opts.ReadOnly || Opts.WriteOnly) {
    UdkHttpServerUse (Server, PermissionMiddleware, &Opts);
  }

  //
  // Register routes (exact root match first, then prefix matches)
  //
  UdkHttpServerAddRoute (Server, "GET",    "/",  HandleGetRoot,   NULL);
  UdkHttpServerAddRoute (Server, "GET",    "/*", HandleGetPath,   NULL);
  UdkHttpServerAddRoute (Server, "PUT",    "/*", HandlePutPath,   NULL);
  UdkHttpServerAddRoute (Server, "DELETE", "/*", HandleDeletePath, NULL);
  UdkHttpServerAddRoute (Server, "POST",   "/*", HandlePostPath,  NULL);

  //
  // Print banner
  //
  Print (L"\nUefiXfer v0.1 \u2014 UEFI HTTP File Server\n");
  Print (L"Listening on %d.%d.%d.%d:%d\n",
    Addr.Addr[0], Addr.Addr[1], Addr.Addr[2], Addr.Addr[3], Opts.Port);

  CONST CHAR16  *Mode = L"read-write";
  if (Opts.ReadOnly) {
    Mode = L"read-only";
  }

  if (Opts.WriteOnly) {
    Mode = L"write-only";
  }

  Print (L"Mode: %s\n", Mode);

  Print (L"Volumes:\n");
  UINTN  VCount = FileTransferGetVolumeCount ();
  for (UINTN I = 0; I < VCount; I++) {
    FT_VOLUME  Vol;
    FileTransferGetVolume (I, &Vol);
    Print (L"  %s:\n", Vol.Name);
  }

  Print (L"Press ESC to stop.\n\n");

  //
  // Run server with event loop (ESC to quit)
  //
  UDK_LOOP  *Loop = UdkLoopNew ();
  if (Loop == NULL) {
    UdkHttpServerFree (Server);
    NetworkCleanup ();
    return EFI_OUT_OF_RESOURCES;
  }

  mServeLoop = Loop;
  UdkLoopAddKeyPress (Loop, EscKeyHandler, NULL);

  Status = UdkHttpServerAttach (Server, Loop);
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: Server attach failed: %r\n", Status);
    UdkLoopFree (Loop);
    UdkHttpServerFree (Server);
    NetworkCleanup ();
    return Status;
  }

  Status = UdkLoopRun (Loop);

  mServeLoop = NULL;
  UdkLoopFree (Loop);
  UdkHttpServerFree (Server);
  NetworkCleanup ();
  return Status;
}
