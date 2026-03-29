/** @file
  HttpFS — serve command handler.

  HTTP file server using AxlNet's AxlHttpServer. Registers route
  handlers that map REST API endpoints to FileTransferLib operations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "HttpFsInternal.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/NetworkLib.h>
#include <Library/FileTransferLib.h>
#include <Library/AxlLib.h>
#include <axl/axl-net.h>

AXL_LOG_DOMAIN ("serve");

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
  IN AxlHttpRequest  *Req
  )
{
  CONST CHAR8  *Accept = (CONST CHAR8 *)AxlHashGet (Req->headers, "accept");

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
PermissionMiddleware (
  AxlHttpRequest   *Req,
  AxlHttpResponse  *Resp,
  VOID               *Data
  )
{
  SERVE_OPTIONS  *Opts = (SERVE_OPTIONS *)Data;

  if (Opts->ReadOnly &&
      (AsciiStrCmp (Req->method, "PUT") == 0 ||
       AsciiStrCmp (Req->method, "DELETE") == 0 ||
       AsciiStrCmp (Req->method, "POST") == 0))
  {
    axl_http_response_set_text (Resp, "Read-only mode\n");
    axl_http_response_set_status (Resp, 403);
    return EFI_ACCESS_DENIED;
  }

  if (Opts->WriteOnly && AsciiStrCmp (Req->method, "GET") == 0) {
    axl_http_response_set_text (Resp, "Write-only mode\n");
    axl_http_response_set_status (Resp, 403);
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
HandleGetRoot (
  AxlHttpRequest   *Req,
  AxlHttpResponse  *Resp,
  VOID               *Data
  )
{
  CHAR8    Buf[4096];
  UINTN    Written = 0;
  BOOLEAN  AsJson = WantsJson (Req);

  (VOID)Data;

  FileTransferListVolumes (AsJson, Buf, sizeof (Buf), &Written);

  if (AsJson) {
    axl_http_response_set_json (Resp, Buf);
  } else {
    Resp->body = AllocatePool (Written);
    if (Resp->body != NULL) {
      CopyMem (Resp->body, Buf, Written);
      Resp->body_size = Written;
    }

    Resp->content_type = "text/html";
    Resp->status_code = 200;
  }

  return EFI_SUCCESS;
}

/// GET /<vol>/<path> — download file or list directory.
STATIC
EFI_STATUS
HandleGetPath (
  AxlHttpRequest   *Req,
  AxlHttpResponse  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;
  BOOLEAN     IsDir;

  (VOID)Data;

  Status = ParseVolumePath (Req->path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Volume not found\n");
    axl_http_response_set_status (Resp, 404);
    return EFI_SUCCESS;
  }

  IsDir = FALSE;
  Status = FileTransferIsDir (&Volume, SubPath, &IsDir);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Not found\n");
    axl_http_response_set_status (Resp, 404);
    return EFI_SUCCESS;
  }

  if (IsDir) {
    CHAR8    Buf[8192];
    UINTN    Written = 0;
    BOOLEAN  AsJson = WantsJson (Req);

    Status = FileTransferListDir (&Volume, SubPath, AsJson, Buf, sizeof (Buf), &Written);
    if (EFI_ERROR (Status)) {
      axl_http_response_set_text (Resp, "Directory listing failed\n");
      axl_http_response_set_status (Resp, 500);
      return EFI_SUCCESS;
    }

    if (AsJson) {
      axl_http_response_set_json (Resp, Buf);
    } else {
      Resp->body = AllocatePool (Written);
      if (Resp->body != NULL) {
        CopyMem (Resp->body, Buf, Written);
        Resp->body_size = Written;
      }

      Resp->content_type = "text/html";
      Resp->status_code = 200;
    }

    return EFI_SUCCESS;
  }

  //
  // File download — read entire file into memory
  //
  UINT64  FileSize = 0;
  Status = FileTransferGetFileSize (&Volume, SubPath, &FileSize);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "File not found\n");
    axl_http_response_set_status (Resp, 404);
    return EFI_SUCCESS;
  }

  UINT64  Offset = 0;
  UINTN   SendSize = (UINTN)FileSize;

  //
  // Handle Range header
  //
  CONST CHAR8  *RangeHdr = (CONST CHAR8 *)AxlHashGet (Req->headers, "range");
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
      Resp->status_code = 206;

      //
      // Set Content-Range header
      //
      if (Resp->headers == NULL) {
        Resp->headers = AxlHashNew ();
      }

      if (Resp->headers != NULL) {
        CHAR8  RangeBuf[128];
        AsciiSPrint (RangeBuf, sizeof (RangeBuf),
          "bytes %Lu-%Lu/%Lu", (UINT64)Start, (UINT64)End, (UINT64)FileSize);
        AxlHashSet (Resp->headers, "content-range", AxlStrDup (RangeBuf));
      }
    }
  }

  //
  // Read file content
  //
  FT_READ_CTX  ReadCtx;
  Status = FileTransferOpenRead (&Volume, SubPath, Offset, NULL, NULL, &ReadCtx);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Cannot open file\n");
    axl_http_response_set_status (Resp, 500);
    return EFI_SUCCESS;
  }

  VOID  *FileBuf = AllocatePool (SendSize);
  if (FileBuf == NULL) {
    FileTransferCloseRead (&ReadCtx);
    axl_http_response_set_text (Resp, "Out of memory\n");
    axl_http_response_set_status (Resp, 500);
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

  Resp->body = FileBuf;
  Resp->body_size = TotalRead;
  Resp->content_type = "application/octet-stream";
  if (Resp->status_code == 0) {
    Resp->status_code = 200;
  }

  return EFI_SUCCESS;
}

/// PUT /<vol>/<path> — upload/overwrite file.
STATIC
EFI_STATUS
HandlePutPath (
  AxlHttpRequest   *Req,
  AxlHttpResponse  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;

  (VOID)Data;

  Status = ParseVolumePath (Req->path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Volume not found\n");
    axl_http_response_set_status (Resp, 404);
    return EFI_SUCCESS;
  }

  FT_WRITE_CTX  WriteCtx;
  Status = FileTransferOpenWrite (&Volume, SubPath, NULL, NULL, &WriteCtx);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Cannot create file\n");
    axl_http_response_set_status (Resp, 500);
    return EFI_SUCCESS;
  }

  //
  // Write request body (already fully read by AxlHttpServer)
  //
  if (Req->body != NULL && Req->body_size > 0) {
    UINTN  Written = 0;
    while (Written < Req->body_size) {
      UINTN  ChunkSize = Req->body_size - Written;
      if (ChunkSize > FT_CHUNK_SIZE) {
        ChunkSize = FT_CHUNK_SIZE;
      }

      Status = FileTransferWriteChunk (&WriteCtx,
                 (CONST UINT8 *)Req->body + Written, ChunkSize);
      if (EFI_ERROR (Status)) {
        break;
      }

      Written += ChunkSize;
    }
  }

  FileTransferCloseWrite (&WriteCtx);
  axl_http_response_set_text (Resp, "Created\n");
  axl_http_response_set_status (Resp, 201);
  return EFI_SUCCESS;
}

/// DELETE /<vol>/<path> — delete file or empty directory.
STATIC
EFI_STATUS
HandleDeletePath (
  AxlHttpRequest   *Req,
  AxlHttpResponse  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;

  (VOID)Data;

  Status = ParseVolumePath (Req->path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Volume not found\n");
    axl_http_response_set_status (Resp, 404);
    return EFI_SUCCESS;
  }

  Status = FileTransferDelete (&Volume, SubPath);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Not found\n");
    axl_http_response_set_status (Resp, 404);
    return EFI_SUCCESS;
  }

  axl_http_response_set_text (Resp, "Deleted\n");
  return EFI_SUCCESS;
}

/// POST /<vol>/<path>?mkdir — create directory.
STATIC
EFI_STATUS
HandlePostPath (
  AxlHttpRequest   *Req,
  AxlHttpResponse  *Resp,
  VOID               *Data
  )
{
  FT_VOLUME   Volume;
  CHAR16      SubPath[512];
  EFI_STATUS  Status;

  (VOID)Data;

  if (Req->query == NULL || AsciiStrCmp (Req->query, "mkdir") != 0) {
    axl_http_response_set_text (Resp, "Use ?mkdir\n");
    axl_http_response_set_status (Resp, 400);
    return EFI_SUCCESS;
  }

  Status = ParseVolumePath (Req->path, &Volume, SubPath, 512);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "Volume not found\n");
    axl_http_response_set_status (Resp, 404);
    return EFI_SUCCESS;
  }

  Status = FileTransferMkdir (&Volume, SubPath);
  if (EFI_ERROR (Status)) {
    axl_http_response_set_text (Resp, "mkdir failed\n");
    axl_http_response_set_status (Resp, 500);
    return EFI_SUCCESS;
  }

  axl_http_response_set_text (Resp, "Created\n");
  axl_http_response_set_status (Resp, 201);
  return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// ESC key handler for the event loop
// ----------------------------------------------------------------------------

STATIC AXL_LOOP  *mServeLoop = NULL;

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
      AxlLoopQuit (mServeLoop);
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
  AxlHttpServer  *Server;

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
      Print (L"Usage: HttpFS serve [options]\n");
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
  Server = axl_http_server_new (Opts.Port);
  if (Server == NULL) {
    Print (L"ERROR: HTTP server creation failed\n");
    NetworkCleanup ();
    return EFI_OUT_OF_RESOURCES;
  }

  axl_http_server_set_body_limit (Server, 128 * 1024 * 1024);  // 128 MB

  //
  // Disable keep-alive (avoids blocking on single-threaded server)
  //
  axl_http_server_set_keep_alive (Server, 0);

  //
  // Permission middleware
  //
  if (Opts.ReadOnly || Opts.WriteOnly) {
    axl_http_server_use (Server, PermissionMiddleware, &Opts);
  }

  //
  // Register routes (exact root match first, then prefix matches)
  //
  axl_http_server_add_route (Server, "GET",    "/",  HandleGetRoot,   NULL);
  axl_http_server_add_route (Server, "GET",    "/*", HandleGetPath,   NULL);
  axl_http_server_add_route (Server, "PUT",    "/*", HandlePutPath,   NULL);
  axl_http_server_add_route (Server, "DELETE", "/*", HandleDeletePath, NULL);
  axl_http_server_add_route (Server, "POST",   "/*", HandlePostPath,  NULL);

  //
  // Print banner
  //
  Print (L"\nHttpFS v0.1 \u2014 UEFI HTTP File Server\n");
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
  AXL_LOOP  *Loop = AxlLoopNew ();
  if (Loop == NULL) {
    axl_http_server_free (Server);
    NetworkCleanup ();
    return EFI_OUT_OF_RESOURCES;
  }

  mServeLoop = Loop;
  AxlLoopAddKeyPress (Loop, EscKeyHandler, NULL);

  Status = axl_http_server_attach (Server, Loop);
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: Server attach failed: %r\n", Status);
    AxlLoopFree (Loop);
    axl_http_server_free (Server);
    NetworkCleanup ();
    return Status;
  }

  Status = AxlLoopRun (Loop);

  mServeLoop = NULL;
  AxlLoopFree (Loop);
  axl_http_server_free (Server);
  NetworkCleanup ();
  return Status;
}
