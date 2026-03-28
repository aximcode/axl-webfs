/** @file
  HttpServerLib — Internal types and declarations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HTTP_SERVER_INTERNAL_H_
#define HTTP_SERVER_INTERNAL_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Protocol/Tcp4.h>
#include <Protocol/ServiceBinding.h>

#include <Library/HttpServerLib.h>

#define HTTP_RECV_BUF_SIZE     8192
#define HTTP_HEADER_BUF_SIZE   2048
#define HTTP_SEND_BUF_SIZE     4096
#define HTTP_SEND_CHUNK_SIZE   (32 * 1024)
#define HTTP_SEND_TIMEOUT_US   (10 * 1000 * 1000)
#define HTTP_SEND_RETRY_US     (5 * 1000 * 1000)
#define HTTP_RECV_TIMEOUT_US   (10 * 1000 * 1000)
#define HTTP_ACCEPT_TIMEOUT_US (1 * 1000 * 1000)

/// Per-connection state.
typedef struct {
    BOOLEAN                Active;
    EFI_HANDLE             TcpHandle;
    EFI_TCP4_PROTOCOL      *Tcp4;

    /// Header accumulation.
    CHAR8                  HeaderBuf[HTTP_HEADER_BUF_SIZE];
    UINTN                  HeaderLen;
    BOOLEAN                HeadersDone;

    /// Parsed request fields.
    CHAR8                  Method[12];
    CHAR8                  Path[512];
    CHAR8                  QueryString[128];
    UINTN                  ContentLength;
    UINTN                  BodyBytesRead;
    CHAR8                  RangeHeader[64];
    CHAR8                  AcceptHeader[64];
    BOOLEAN                KeepAlive;
} HTTP_SERVER_CONN;

/// Listener state.
typedef struct {
    EFI_HANDLE                     TcpHandle;
    EFI_TCP4_PROTOCOL              *Tcp4;
    EFI_SERVICE_BINDING_PROTOCOL   *TcpSb;
    EFI_HANDLE                     SbHandle;
    BOOLEAN                        Active;
} HTTP_SERVER_LISTENER;

/// Route entry.
typedef struct {
    CONST CHAR8          *Method;
    CONST CHAR8          *Path;
    UINTN                PathLen;
    HTTP_ROUTE_HANDLER   Handler;
    UINT32               Flags;
} HTTP_ROUTE_ENTRY;

#endif // HTTP_SERVER_INTERNAL_H_
