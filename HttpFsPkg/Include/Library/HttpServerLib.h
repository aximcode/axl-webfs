/** @file
  HttpServerLib — HTTP/1.1 server with cooperative polling.

  TCP listener, 4-slot connection pool, request parsing, route dispatch,
  and streaming response/upload API. Adapted from SoftBMC's HTTP server,
  stripped of WebSocket, TLS, auth, and response cache.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HTTP_SERVER_LIB_H_
#define HTTP_SERVER_LIB_H_

#include <Uefi.h>

#define HTTP_SERVER_MAX_CONNECTIONS  4
#define HTTP_SERVER_MAX_ROUTES      16

/// Route permission flags.
#define HTTP_ROUTE_FLAG_READ   0x01   ///< Active unless --write-only.
#define HTTP_ROUTE_FLAG_WRITE  0x02   ///< Active unless --read-only.
#define HTTP_ROUTE_FLAG_PREFIX 0x04   ///< Prefix match instead of exact.

/// Parsed HTTP request (passed to route handlers).
typedef struct {
    CONST CHAR8  *Method;
    CONST CHAR8  *Path;
    CONST CHAR8  *QueryString;
    UINTN        ContentLength;     ///< MAX_UINTN if unknown.
    CONST CHAR8  *RangeHeader;      ///< Raw Range value, or NULL.
    CONST CHAR8  *AcceptHeader;     ///< Raw Accept value, or NULL.
} HTTP_SERVER_REQUEST;

/// Route handler callback. Called from within HttpServerPoll().
typedef EFI_STATUS (*HTTP_ROUTE_HANDLER)(IN HTTP_SERVER_REQUEST *Request);

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize the HTTP server. Creates a passive TCP4 listener on Port.
EFI_STATUS EFIAPI HttpServerInit (IN EFI_HANDLE ImageHandle, IN EFI_HANDLE TcpSbHandle, IN UINT16 Port);

/// Poll for connections and data. Call from the main loop. Returns TRUE if server is active.
BOOLEAN EFIAPI HttpServerPoll (VOID);

/// Stop the server. Disconnects all clients, tears down the listener.
VOID EFIAPI HttpServerStop (VOID);

// ============================================================================
// Route registration (call before first Poll)
// ============================================================================

EFI_STATUS EFIAPI HttpServerRegisterRoute (IN CONST CHAR8 *Method OPTIONAL, IN CONST CHAR8 *Path, IN HTTP_ROUTE_HANDLER Handler, IN UINT32 Flags);

/// Set permission mode. Routes with FLAG_WRITE are disabled in read-only mode,
/// routes with FLAG_READ are disabled in write-only mode.
VOID EFIAPI HttpServerSetPermissions (IN BOOLEAN ReadOnly, IN BOOLEAN WriteOnly);

// ============================================================================
// Response API (call from within a route handler)
// ============================================================================

/// Send a complete response (headers + body).
EFI_STATUS EFIAPI HttpServerSendResponse (IN UINT16 StatusCode, IN CONST CHAR8 *ContentType, IN CONST VOID *Body, IN UINTN BodyLen);

/// Send response headers only (for streaming downloads).
EFI_STATUS EFIAPI HttpServerSendHeaders (IN UINT16 StatusCode, IN CONST CHAR8 *ContentType, IN UINTN ContentLength, IN CONST CHAR8 *ExtraHeaders OPTIONAL);

/// Send a chunk of response body (after SendHeaders).
EFI_STATUS EFIAPI HttpServerSendChunk (IN CONST VOID *Data, IN UINTN Len);

/// Send an error response (plain text body).
VOID EFIAPI HttpServerSendError (IN UINT16 StatusCode, IN CONST CHAR8 *Message);

// ============================================================================
// Streaming upload body read (call from PUT/POST handler)
// ============================================================================

/// Read upload body data. Returns 0 bytes at end of body.
EFI_STATUS EFIAPI HttpServerRecvBody (OUT VOID *Buffer, IN UINTN BufferSize, OUT UINTN *BytesRead);

/// Get number of active connections.
UINTN EFIAPI HttpServerGetActiveConnections (VOID);

#endif // HTTP_SERVER_LIB_H_
