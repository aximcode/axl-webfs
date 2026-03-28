/** @file
  HttpClientLib — HTTP/1.1 client over TCP4.

  Provides persistent TCP connections, HTTP request building, response
  parsing, and streaming body reads. Designed for talking to xfer-server.py
  (mount command) and general HTTP endpoints.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HTTP_CLIENT_LIB_H_
#define HTTP_CLIENT_LIB_H_

#include <Uefi.h>
#include <Protocol/Tcp4.h>
#include <Protocol/ServiceBinding.h>

/// Maximum number of response headers parsed.
#define HTTP_MAX_HEADERS  32

/// HTTP methods supported by the client.
#define HTTP_METHOD_GET     "GET"
#define HTTP_METHOD_PUT     "PUT"
#define HTTP_METHOD_POST    "POST"
#define HTTP_METHOD_DELETE  "DELETE"

/// A single HTTP header (name: value pair).
typedef struct {
    CONST CHAR8  *Name;
    CONST CHAR8  *Value;
} HTTP_HEADER;

/// Opaque HTTP client connection.
typedef struct {
    EFI_HANDLE                     TcpChildHandle;
    EFI_TCP4_PROTOCOL              *Tcp4;
    EFI_SERVICE_BINDING_PROTOCOL   *TcpSb;
    EFI_HANDLE                     SbHandle;
    EFI_IPv4_ADDRESS               ServerAddr;
    UINT16                         ServerPort;
    BOOLEAN                        Connected;
    /// Internal receive buffer for header parsing.
    CHAR8                          RecvBuf[4096];
    UINTN                          RecvBufLen;
    UINTN                          RecvBufPos;
} HTTP_CLIENT;

/// Parsed HTTP response context (returned by HttpClientRequest).
typedef struct {
    UINT16       StatusCode;
    UINTN        ContentLength;    ///< SIZE_MAX if unknown/chunked.
    BOOLEAN      Chunked;
    UINTN        BodyBytesRead;    ///< Bytes of body consumed so far.
    /// Parsed response headers.
    HTTP_HEADER  Headers[HTTP_MAX_HEADERS];
    UINTN        HeaderCount;
    /// Raw header block (headers point into this buffer).
    CHAR8        HeaderBuf[2048];
} HTTP_RESPONSE_CTX;

/**
  Connect to an HTTP server.

  Creates a TCP4 child on the NIC handle from NetworkLib, configures it,
  and establishes a TCP connection to ServerAddr:Port.

  @param[in]  ImageHandle  The caller's image handle.
  @param[in]  SbHandle     TCP4 ServiceBinding handle (from NetworkGetTcpServiceBinding).
  @param[in]  ServerAddr   Server IPv4 address.
  @param[in]  Port         Server TCP port.
  @param[in]  TimeoutMs    Connection timeout in milliseconds.
  @param[out] Client       Initialized client context.

  @retval EFI_SUCCESS      Connected.
  @retval EFI_TIMEOUT      Connection timed out.
  @retval EFI_DEVICE_ERROR TCP error.
**/
EFI_STATUS
EFIAPI
HttpClientConnect (
    IN  EFI_HANDLE        ImageHandle,
    IN  EFI_HANDLE        SbHandle,
    IN  EFI_IPv4_ADDRESS  ServerAddr,
    IN  UINT16            Port,
    IN  UINTN             TimeoutMs,
    OUT HTTP_CLIENT       *Client
    );

/**
  Send an HTTP request and parse the response status + headers.

  Builds an HTTP/1.1 request line and headers, sends them over the
  existing TCP connection, then reads and parses the response status
  line and headers. The response body is NOT read — use
  HttpClientReadBody() to stream it.

  @param[in]  Client       Connected client context.
  @param[in]  Method       HTTP method string (e.g., HTTP_METHOD_GET).
  @param[in]  Path         Request path (e.g., "/files/test.efi").
  @param[in]  Headers      Additional request headers, or NULL.
  @param[in]  HeaderCount  Number of additional headers.
  @param[in]  Body         Request body for PUT/POST, or NULL.
  @param[in]  BodyLen      Length of request body.
  @param[out] Response     Parsed response context.

  @retval EFI_SUCCESS           Response headers parsed.
  @retval EFI_CONNECTION_RESET  Server closed connection (reconnect needed).
  @retval EFI_DEVICE_ERROR      TCP send/receive error.
**/
EFI_STATUS
EFIAPI
HttpClientRequest (
    IN  HTTP_CLIENT        *Client,
    IN  CONST CHAR8        *Method,
    IN  CONST CHAR8        *Path,
    IN  HTTP_HEADER        *Headers      OPTIONAL,
    IN  UINTN              HeaderCount,
    IN  CONST VOID         *Body         OPTIONAL,
    IN  UINTN              BodyLen,
    OUT HTTP_RESPONSE_CTX  *Response
    );

/**
  Read response body data (streaming).

  Reads up to BufferSize bytes of the response body. Call repeatedly
  until *BytesRead == 0 (end of body). Handles both Content-Length
  and chunked transfer encoding.

  @param[in]  Client       Connected client context.
  @param[in]  Response     Response context from HttpClientRequest.
  @param[out] Buffer       Caller-provided buffer for body data.
  @param[in]  BufferSize   Size of Buffer in bytes.
  @param[out] BytesRead    Number of bytes actually read (0 = EOF).

  @retval EFI_SUCCESS      Data read (or EOF if *BytesRead == 0).
  @retval EFI_DEVICE_ERROR TCP receive error.
**/
EFI_STATUS
EFIAPI
HttpClientReadBody (
    IN  HTTP_CLIENT        *Client,
    IN  HTTP_RESPONSE_CTX  *Response,
    OUT VOID               *Buffer,
    IN  UINTN              BufferSize,
    OUT UINTN              *BytesRead
    );

/**
  Find a response header by name (case-insensitive).

  @param[in] Response    Parsed response context.
  @param[in] Name        Header name to find.

  @return Pointer to the header value string, or NULL if not found.
**/
CONST CHAR8 *
EFIAPI
HttpClientGetHeader (
    IN CONST HTTP_RESPONSE_CTX  *Response,
    IN CONST CHAR8              *Name
    );

/**
  Close the HTTP client connection and release resources.

  @param[in] Client  Client context to close.
**/
VOID
EFIAPI
HttpClientClose (
    IN HTTP_CLIENT  *Client
    );

#endif // HTTP_CLIENT_LIB_H_
