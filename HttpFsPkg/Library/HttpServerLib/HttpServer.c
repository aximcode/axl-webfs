/** @file
  HttpServerLib — HTTP/1.1 server with cooperative polling.

  TCP listener, 4-slot connection pool, request parsing, route dispatch,
  and streaming response/upload API. Adapted from SoftBMC HttpServer.c.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "HttpServerInternal.h"

// ----------------------------------------------------------------------------
// Module state
// ----------------------------------------------------------------------------

static EFI_HANDLE           mImageHandle = NULL;
static HTTP_SERVER_LISTENER mListener;
static HTTP_SERVER_CONN     mConns[HTTP_SERVER_MAX_CONNECTIONS];
static HTTP_ROUTE_ENTRY     mRoutes[HTTP_SERVER_MAX_ROUTES];
static UINTN                mRouteCount = 0;
static HTTP_SERVER_CONN     *mCurrentConn = NULL;
static BOOLEAN              mReadOnly = FALSE;
static BOOLEAN              mWriteOnly = FALSE;

// Forward declarations
static VOID DisconnectClient(HTTP_SERVER_CONN *Conn);
static EFI_STATUS TcpSendAll(EFI_TCP4_PROTOCOL *Tcp4, VOID *Buffer, UINTN Length);
static EFI_STATUS TcpRecvSome(EFI_TCP4_PROTOCOL *Tcp4, UINT8 *Buffer, UINTN MaxLen, UINTN *Received, UINTN TimeoutUs);

// ----------------------------------------------------------------------------
// TCP helpers (adapted from HttpClientLib)
// ----------------------------------------------------------------------------

static EFI_STATUS TcpSendChunk(
    IN EFI_TCP4_PROTOCOL *Tcp4, IN VOID *Buffer, IN UINTN Length
) {
    EFI_EVENT Event = NULL;
    EFI_STATUS Status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &Event);
    if (EFI_ERROR(Status)) return Status;

    EFI_TCP4_FRAGMENT_DATA Fragment;
    Fragment.FragmentLength = (UINT32)Length;
    Fragment.FragmentBuffer = Buffer;

    EFI_TCP4_TRANSMIT_DATA TxData;
    SetMem(&TxData, sizeof(TxData), 0);
    TxData.Push = TRUE;
    TxData.DataLength = (UINT32)Length;
    TxData.FragmentCount = 1;
    TxData.FragmentTable[0] = Fragment;

    EFI_TCP4_IO_TOKEN TxToken;
    SetMem(&TxToken, sizeof(TxToken), 0);
    TxToken.CompletionToken.Event = Event;
    TxToken.CompletionToken.Status = EFI_ABORTED;
    TxToken.Packet.TxData = &TxData;

    Status = Tcp4->Transmit(Tcp4, &TxToken);
    if (Status == EFI_NOT_READY) {
        UINTN Elapsed = 0;
        while (Elapsed < HTTP_SEND_RETRY_US) {
            Tcp4->Poll(Tcp4);
            gBS->Stall(5000);
            Elapsed += 5000;
            Status = Tcp4->Transmit(Tcp4, &TxToken);
            if (Status != EFI_NOT_READY) break;
        }
    }
    if (EFI_ERROR(Status)) { gBS->CloseEvent(Event); return Status; }

    UINTN Elapsed = 0;
    while (Elapsed < HTTP_SEND_TIMEOUT_US) {
        Tcp4->Poll(Tcp4);
        if (gBS->CheckEvent(Event) == EFI_SUCCESS) {
            Status = TxToken.CompletionToken.Status;
            gBS->CloseEvent(Event);
            return Status;
        }
        gBS->Stall(100);
        Elapsed += 100;
    }

    Tcp4->Cancel(Tcp4, &TxToken.CompletionToken);
    gBS->CloseEvent(Event);
    return EFI_TIMEOUT;
}

static EFI_STATUS TcpSendAll(EFI_TCP4_PROTOCOL *Tcp4, VOID *Buffer, UINTN Length) {
    UINT8 *Ptr = (UINT8 *)Buffer;
    UINTN Remaining = Length;
    while (Remaining > 0) {
        UINTN Chunk = (Remaining > HTTP_SEND_CHUNK_SIZE) ? HTTP_SEND_CHUNK_SIZE : Remaining;
        EFI_STATUS Status = TcpSendChunk(Tcp4, Ptr, Chunk);
        if (EFI_ERROR(Status)) return Status;
        Ptr += Chunk;
        Remaining -= Chunk;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS TcpRecvSome(
    EFI_TCP4_PROTOCOL *Tcp4, UINT8 *Buffer, UINTN MaxLen, UINTN *Received, UINTN TimeoutUs
) {
    *Received = 0;
    EFI_EVENT RxEvent = NULL;
    EFI_STATUS Status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &RxEvent);
    if (EFI_ERROR(Status)) return Status;

    EFI_TCP4_RECEIVE_DATA RxData;
    SetMem(&RxData, sizeof(RxData), 0);
    RxData.DataLength = (UINT32)MaxLen;
    RxData.FragmentCount = 1;
    RxData.FragmentTable[0].FragmentLength = (UINT32)MaxLen;
    RxData.FragmentTable[0].FragmentBuffer = Buffer;

    EFI_TCP4_IO_TOKEN RxToken;
    SetMem(&RxToken, sizeof(RxToken), 0);
    RxToken.CompletionToken.Event = RxEvent;
    RxToken.CompletionToken.Status = EFI_ABORTED;
    RxToken.Packet.RxData = &RxData;

    Status = Tcp4->Receive(Tcp4, &RxToken);
    if (EFI_ERROR(Status)) { gBS->CloseEvent(RxEvent); return Status; }

    UINTN Elapsed = 0;
    while (Elapsed < TimeoutUs) {
        Tcp4->Poll(Tcp4);
        if (gBS->CheckEvent(RxEvent) == EFI_SUCCESS) {
            Status = RxToken.CompletionToken.Status;
            if (!EFI_ERROR(Status)) *Received = RxData.DataLength;
            gBS->CloseEvent(RxEvent);
            return Status;
        }
        gBS->Stall(1000);
        Elapsed += 1000;
    }

    Tcp4->Cancel(Tcp4, &RxToken.CompletionToken);
    gBS->CloseEvent(RxEvent);
    return EFI_TIMEOUT;
}

// ----------------------------------------------------------------------------
// Listener
// ----------------------------------------------------------------------------

static EFI_STATUS InitListener(IN EFI_HANDLE SbHandle, IN UINT16 Port) {
    SetMem(&mListener, sizeof(mListener), 0);
    mListener.SbHandle = SbHandle;

    EFI_STATUS Status = gBS->OpenProtocol(
        SbHandle, &gEfiTcp4ServiceBindingProtocolGuid,
        (VOID **)&mListener.TcpSb,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) return Status;

    // Create listener TCP4 child
    Status = mListener.TcpSb->CreateChild(mListener.TcpSb, &mListener.TcpHandle);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->OpenProtocol(
        mListener.TcpHandle, &gEfiTcp4ProtocolGuid,
        (VOID **)&mListener.Tcp4,
        mImageHandle, mListener.TcpHandle,
        EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) return Status;

    // Configure as passive (server) listener
    EFI_TCP4_ACCESS_POINT AccessPoint;
    SetMem(&AccessPoint, sizeof(AccessPoint), 0);
    AccessPoint.ActiveFlag = FALSE;
    AccessPoint.UseDefaultAddress = TRUE;
    AccessPoint.StationPort = Port;

    EFI_TCP4_CONFIG_DATA Config;
    SetMem(&Config, sizeof(Config), 0);
    Config.TimeToLive = 64;
    Config.AccessPoint = AccessPoint;

    Status = mListener.Tcp4->Configure(mListener.Tcp4, &Config);
    if (EFI_ERROR(Status)) return Status;

    mListener.Active = TRUE;
    return EFI_SUCCESS;
}

/// Accept a new connection. Non-blocking — returns immediately.
static EFI_STATUS AcceptOne(OUT EFI_HANDLE *NewHandle) {
    EFI_EVENT AccEvent = NULL;
    EFI_STATUS Status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &AccEvent);
    if (EFI_ERROR(Status)) return Status;

    EFI_TCP4_LISTEN_TOKEN AccToken;
    SetMem(&AccToken, sizeof(AccToken), 0);
    AccToken.CompletionToken.Event = AccEvent;
    AccToken.CompletionToken.Status = EFI_ABORTED;

    Status = mListener.Tcp4->Accept(mListener.Tcp4, &AccToken);
    if (EFI_ERROR(Status)) {
        gBS->CloseEvent(AccEvent);
        return Status;
    }

    // Brief poll for accept completion
    UINTN Elapsed = 0;
    while (Elapsed < HTTP_ACCEPT_TIMEOUT_US) {
        mListener.Tcp4->Poll(mListener.Tcp4);
        if (gBS->CheckEvent(AccEvent) == EFI_SUCCESS) {
            Status = AccToken.CompletionToken.Status;
            gBS->CloseEvent(AccEvent);
            if (!EFI_ERROR(Status)) {
                *NewHandle = AccToken.NewChildHandle;
            }
            return Status;
        }
        gBS->Stall(1000);
        Elapsed += 1000;
    }

    mListener.Tcp4->Cancel(mListener.Tcp4, &AccToken.CompletionToken);
    gBS->CloseEvent(AccEvent);
    return EFI_TIMEOUT;
}

// ----------------------------------------------------------------------------
// Connection management
// ----------------------------------------------------------------------------

static HTTP_SERVER_CONN * FindFreeSlot(VOID) {
    for (UINTN i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
        if (!mConns[i].Active) return &mConns[i];
    }
    return NULL;
}

static VOID DisconnectClient(HTTP_SERVER_CONN *Conn) {
    if (!Conn->Active) return;

    if (Conn->Tcp4 != NULL) {
        Conn->Tcp4->Configure(Conn->Tcp4, NULL);
        gBS->CloseProtocol(Conn->TcpHandle, &gEfiTcp4ProtocolGuid,
                           mImageHandle, Conn->TcpHandle);
    }
    if (mListener.TcpSb != NULL && Conn->TcpHandle != NULL) {
        mListener.TcpSb->DestroyChild(mListener.TcpSb, Conn->TcpHandle);
    }

    SetMem(Conn, sizeof(*Conn), 0);
}

// ----------------------------------------------------------------------------
// HTTP parsing
// ----------------------------------------------------------------------------

/// Case-insensitive ASCII comparison for N bytes.
static BOOLEAN AsciiCmpI(CONST CHAR8 *A, CONST CHAR8 *B, UINTN Len) {
    for (UINTN i = 0; i < Len; i++) {
        CHAR8 Ca = A[i], Cb = B[i];
        if (Ca >= 'A' && Ca <= 'Z') Ca += 32;
        if (Cb >= 'A' && Cb <= 'Z') Cb += 32;
        if (Ca != Cb) return FALSE;
    }
    return TRUE;
}

/// Parse request line and headers from HeaderBuf.
static VOID ParseRequest(HTTP_SERVER_CONN *Conn) {
    CHAR8 *Buf = Conn->HeaderBuf;
    UINTN Len = Conn->HeaderLen;

    // Parse request line: METHOD PATH HTTP/1.x\r\n
    UINTN MethodEnd = 0;
    while (MethodEnd < Len && Buf[MethodEnd] != ' ') MethodEnd++;
    UINTN CopyLen = (MethodEnd < sizeof(Conn->Method) - 1) ? MethodEnd : sizeof(Conn->Method) - 1;
    CopyMem(Conn->Method, Buf, CopyLen);
    Conn->Method[CopyLen] = '\0';

    UINTN PathStart = MethodEnd + 1;
    UINTN PathEnd = PathStart;
    while (PathEnd < Len && Buf[PathEnd] != ' ' && Buf[PathEnd] != '?') PathEnd++;

    CopyLen = PathEnd - PathStart;
    if (CopyLen >= sizeof(Conn->Path)) CopyLen = sizeof(Conn->Path) - 1;
    CopyMem(Conn->Path, Buf + PathStart, CopyLen);
    Conn->Path[CopyLen] = '\0';

    // Query string
    Conn->QueryString[0] = '\0';
    if (PathEnd < Len && Buf[PathEnd] == '?') {
        UINTN QsStart = PathEnd + 1;
        UINTN QsEnd = QsStart;
        while (QsEnd < Len && Buf[QsEnd] != ' ') QsEnd++;
        CopyLen = QsEnd - QsStart;
        if (CopyLen >= sizeof(Conn->QueryString)) CopyLen = sizeof(Conn->QueryString) - 1;
        CopyMem(Conn->QueryString, Buf + QsStart, CopyLen);
        Conn->QueryString[CopyLen] = '\0';
    }

    // Defaults
    Conn->ContentLength = MAX_UINTN;
    Conn->KeepAlive = TRUE;
    Conn->RangeHeader[0] = '\0';
    Conn->AcceptHeader[0] = '\0';

    // Parse headers
    UINTN Pos = 0;
    while (Pos < Len && !(Buf[Pos] == '\r' && Pos + 1 < Len && Buf[Pos + 1] == '\n')) Pos++;
    Pos += 2;  // Skip request line

    while (Pos < Len) {
        if (Buf[Pos] == '\r') break;  // End of headers

        UINTN LineEnd = Pos;
        while (LineEnd < Len && Buf[LineEnd] != '\r') LineEnd++;

        // Find colon
        UINTN Colon = Pos;
        while (Colon < LineEnd && Buf[Colon] != ':') Colon++;
        if (Colon < LineEnd) {
            UINTN NameLen = Colon - Pos;
            UINTN ValStart = Colon + 1;
            while (ValStart < LineEnd && Buf[ValStart] == ' ') ValStart++;
            UINTN ValLen = LineEnd - ValStart;

            if (NameLen == 14 && AsciiCmpI(Buf + Pos, "content-length", 14)) {
                UINTN Val = 0;
                for (UINTN j = ValStart; j < LineEnd; j++) {
                    if (Buf[j] >= '0' && Buf[j] <= '9') Val = Val * 10 + (Buf[j] - '0');
                }
                Conn->ContentLength = Val;
            } else if (NameLen == 5 && AsciiCmpI(Buf + Pos, "range", 5)) {
                CopyLen = (ValLen < sizeof(Conn->RangeHeader) - 1) ? ValLen : sizeof(Conn->RangeHeader) - 1;
                CopyMem(Conn->RangeHeader, Buf + ValStart, CopyLen);
                Conn->RangeHeader[CopyLen] = '\0';
            } else if (NameLen == 6 && AsciiCmpI(Buf + Pos, "accept", 6)) {
                CopyLen = (ValLen < sizeof(Conn->AcceptHeader) - 1) ? ValLen : sizeof(Conn->AcceptHeader) - 1;
                CopyMem(Conn->AcceptHeader, Buf + ValStart, CopyLen);
                Conn->AcceptHeader[CopyLen] = '\0';
            } else if (NameLen == 10 && AsciiCmpI(Buf + Pos, "connection", 10)) {
                if (ValLen == 5 && AsciiCmpI(Buf + ValStart, "close", 5)) {
                    Conn->KeepAlive = FALSE;
                }
            }
        }

        Pos = LineEnd + 2;  // Skip \r\n
    }

    Conn->BodyBytesRead = 0;
    Conn->HeadersDone = TRUE;
}

/// Dispatch to matching route handler.
static EFI_STATUS DispatchRoute(HTTP_SERVER_CONN *Conn) {
    mCurrentConn = Conn;

    HTTP_SERVER_REQUEST Req;
    Req.Method = Conn->Method;
    Req.Path = Conn->Path;
    Req.QueryString = (Conn->QueryString[0] != '\0') ? Conn->QueryString : NULL;
    Req.ContentLength = Conn->ContentLength;
    Req.RangeHeader = (Conn->RangeHeader[0] != '\0') ? Conn->RangeHeader : NULL;
    Req.AcceptHeader = (Conn->AcceptHeader[0] != '\0') ? Conn->AcceptHeader : NULL;

    BOOLEAN PermissionBlocked = FALSE;

    for (UINTN i = 0; i < mRouteCount; i++) {
        HTTP_ROUTE_ENTRY *R = &mRoutes[i];

        // Match method
        if (R->Method != NULL && AsciiStrCmp(R->Method, Conn->Method) != 0) continue;

        // Match path
        if (R->Flags & HTTP_ROUTE_FLAG_PREFIX) {
            if (AsciiStrnCmp(Conn->Path, R->Path, R->PathLen) != 0) continue;
        } else {
            if (AsciiStrCmp(Conn->Path, R->Path) != 0) continue;
        }

        // Route matches — check permission flags
        if ((R->Flags & HTTP_ROUTE_FLAG_WRITE) && mReadOnly) {
            PermissionBlocked = TRUE;
            continue;
        }
        if ((R->Flags & HTTP_ROUTE_FLAG_READ) && mWriteOnly) {
            PermissionBlocked = TRUE;
            continue;
        }

        EFI_STATUS Status = R->Handler(&Req);
        mCurrentConn = NULL;
        return Status;
    }

    // Route matched but blocked by permissions
    if (PermissionBlocked) {
        HttpServerSendError(403, "Forbidden");
    } else {
        HttpServerSendError(404, "Not Found");
    }
    mCurrentConn = NULL;
    return EFI_NOT_FOUND;
}

// ----------------------------------------------------------------------------
// Public API: lifecycle
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI HttpServerInit(
    IN EFI_HANDLE ImageHandle, IN EFI_HANDLE TcpSbHandle, IN UINT16 Port
) {
    mImageHandle = ImageHandle;
    SetMem(mConns, sizeof(mConns), 0);
    SetMem(mRoutes, sizeof(mRoutes), 0);
    mRouteCount = 0;

    return InitListener(TcpSbHandle, Port);
}

BOOLEAN EFIAPI HttpServerPoll(VOID) {
    if (!mListener.Active) return FALSE;

    // Try to accept new connections
    HTTP_SERVER_CONN *FreeSlot = FindFreeSlot();
    if (FreeSlot != NULL) {
        EFI_HANDLE NewHandle = NULL;
        EFI_STATUS Status = AcceptOne(&NewHandle);
        if (!EFI_ERROR(Status) && NewHandle != NULL) {
            SetMem(FreeSlot, sizeof(*FreeSlot), 0);
            FreeSlot->TcpHandle = NewHandle;
            FreeSlot->Active = TRUE;

            gBS->OpenProtocol(
                NewHandle, &gEfiTcp4ProtocolGuid,
                (VOID **)&FreeSlot->Tcp4,
                mImageHandle, NewHandle,
                EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        }
    }

    // Poll each active connection for data
    for (UINTN i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
        HTTP_SERVER_CONN *Conn = &mConns[i];
        if (!Conn->Active || Conn->Tcp4 == NULL) continue;

        if (Conn->HeadersDone) continue;  // Already dispatched, waiting for next request

        // Try to receive header data
        UINT8 TmpBuf[1024];
        UINTN Got = 0;
        EFI_STATUS Status = TcpRecvSome(
            Conn->Tcp4, TmpBuf, sizeof(TmpBuf), &Got, 1000);  // 1ms timeout

        if (Status == EFI_CONNECTION_FIN || Status == EFI_CONNECTION_RESET) {
            DisconnectClient(Conn);
            continue;
        }

        if (Got > 0) {
            UINTN Space = sizeof(Conn->HeaderBuf) - Conn->HeaderLen;
            UINTN Copy = (Got < Space) ? Got : Space;
            CopyMem(Conn->HeaderBuf + Conn->HeaderLen, TmpBuf, Copy);
            Conn->HeaderLen += Copy;

            // Check for end of headers
            for (UINTN j = 0; j + 3 < Conn->HeaderLen; j++) {
                if (Conn->HeaderBuf[j] == '\r' && Conn->HeaderBuf[j + 1] == '\n' &&
                    Conn->HeaderBuf[j + 2] == '\r' && Conn->HeaderBuf[j + 3] == '\n') {
                    // Headers complete
                    ParseRequest(Conn);
                    DispatchRoute(Conn);

                    // Reset for next request (keep-alive) or disconnect
                    if (Conn->KeepAlive && Conn->Active) {
                        Conn->HeaderLen = 0;
                        Conn->HeadersDone = FALSE;
                    } else {
                        DisconnectClient(Conn);
                    }
                    break;
                }
            }
        }
    }

    return TRUE;
}

VOID EFIAPI HttpServerStop(VOID) {
    for (UINTN i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
        DisconnectClient(&mConns[i]);
    }

    if (mListener.Active) {
        if (mListener.Tcp4 != NULL) {
            mListener.Tcp4->Configure(mListener.Tcp4, NULL);
            gBS->CloseProtocol(mListener.TcpHandle, &gEfiTcp4ProtocolGuid,
                               mImageHandle, mListener.TcpHandle);
        }
        if (mListener.TcpSb != NULL) {
            mListener.TcpSb->DestroyChild(mListener.TcpSb, mListener.TcpHandle);
        }
        mListener.Active = FALSE;
    }
}

// ----------------------------------------------------------------------------
// Public API: routes
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI HttpServerRegisterRoute(
    IN CONST CHAR8 *Method OPTIONAL, IN CONST CHAR8 *Path,
    IN HTTP_ROUTE_HANDLER Handler, IN UINT32 Flags
) {
    if (Path == NULL || Handler == NULL) return EFI_INVALID_PARAMETER;
    if (mRouteCount >= HTTP_SERVER_MAX_ROUTES) return EFI_OUT_OF_RESOURCES;

    HTTP_ROUTE_ENTRY *E = &mRoutes[mRouteCount++];
    E->Method = Method;
    E->Path = Path;
    E->PathLen = AsciiStrLen(Path);
    E->Handler = Handler;
    E->Flags = Flags;
    return EFI_SUCCESS;
}

VOID EFIAPI HttpServerSetPermissions(IN BOOLEAN ReadOnly, IN BOOLEAN WriteOnly) {
    mReadOnly = ReadOnly;
    mWriteOnly = WriteOnly;
}

// ----------------------------------------------------------------------------
// Public API: response
// ----------------------------------------------------------------------------

static CONST CHAR8 * StatusText(UINT16 Code) {
    switch (Code) {
        case 200: return "OK";
        case 201: return "Created";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

EFI_STATUS EFIAPI HttpServerSendResponse(
    IN UINT16 StatusCode, IN CONST CHAR8 *ContentType,
    IN CONST VOID *Body, IN UINTN BodyLen
) {
    if (mCurrentConn == NULL || !mCurrentConn->Active) return EFI_NOT_READY;

    CHAR8 Header[HTTP_SEND_BUF_SIZE];
    UINTN Len = AsciiSPrint(Header, sizeof(Header),
        "HTTP/1.1 %d %a\r\nContent-Type: %a\r\nContent-Length: %d\r\n"
        "Connection: %a\r\n\r\n",
        StatusCode, StatusText(StatusCode), ContentType, BodyLen,
        mCurrentConn->KeepAlive ? "keep-alive" : "close");

    EFI_STATUS Status = TcpSendAll(mCurrentConn->Tcp4, Header, Len);
    if (EFI_ERROR(Status)) return Status;

    if (Body != NULL && BodyLen > 0) {
        Status = TcpSendAll(mCurrentConn->Tcp4, (VOID *)Body, BodyLen);
    }
    return Status;
}

EFI_STATUS EFIAPI HttpServerSendHeaders(
    IN UINT16 StatusCode, IN CONST CHAR8 *ContentType,
    IN UINTN ContentLength, IN CONST CHAR8 *ExtraHeaders OPTIONAL
) {
    if (mCurrentConn == NULL || !mCurrentConn->Active) return EFI_NOT_READY;

    CHAR8 Header[HTTP_SEND_BUF_SIZE];
    UINTN Len;

    if (ContentLength != MAX_UINTN) {
        Len = AsciiSPrint(Header, sizeof(Header),
            "HTTP/1.1 %d %a\r\nContent-Type: %a\r\nContent-Length: %d\r\n"
            "Connection: %a\r\n",
            StatusCode, StatusText(StatusCode), ContentType, ContentLength,
            mCurrentConn->KeepAlive ? "keep-alive" : "close");
    } else {
        Len = AsciiSPrint(Header, sizeof(Header),
            "HTTP/1.1 %d %a\r\nContent-Type: %a\r\n"
            "Connection: close\r\n",
            StatusCode, StatusText(StatusCode), ContentType);
        mCurrentConn->KeepAlive = FALSE;
    }

    if (ExtraHeaders != NULL) {
        Len += AsciiSPrint(Header + Len, sizeof(Header) - Len, "%a", ExtraHeaders);
    }
    Len += AsciiSPrint(Header + Len, sizeof(Header) - Len, "\r\n");

    return TcpSendAll(mCurrentConn->Tcp4, Header, Len);
}

EFI_STATUS EFIAPI HttpServerSendChunk(IN CONST VOID *Data, IN UINTN Len) {
    if (mCurrentConn == NULL || !mCurrentConn->Active) return EFI_NOT_READY;
    return TcpSendAll(mCurrentConn->Tcp4, (VOID *)Data, Len);
}

VOID EFIAPI HttpServerSendError(IN UINT16 StatusCode, IN CONST CHAR8 *Message) {
    HttpServerSendResponse(StatusCode, "text/plain", Message, AsciiStrLen(Message));
}

// ----------------------------------------------------------------------------
// Public API: streaming upload body
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI HttpServerRecvBody(
    OUT VOID *Buffer, IN UINTN BufferSize, OUT UINTN *BytesRead
) {
    if (mCurrentConn == NULL || !mCurrentConn->Active) return EFI_NOT_READY;

    *BytesRead = 0;

    // Check if body is complete
    if (mCurrentConn->ContentLength != MAX_UINTN &&
        mCurrentConn->BodyBytesRead >= mCurrentConn->ContentLength) {
        return EFI_SUCCESS;
    }

    UINTN Want = BufferSize;
    if (mCurrentConn->ContentLength != MAX_UINTN) {
        UINTN Remaining = mCurrentConn->ContentLength - mCurrentConn->BodyBytesRead;
        if (Want > Remaining) Want = Remaining;
    }
    if (Want == 0) return EFI_SUCCESS;

    UINTN Got = 0;
    EFI_STATUS Status = TcpRecvSome(
        mCurrentConn->Tcp4, (UINT8 *)Buffer, Want, &Got, HTTP_RECV_TIMEOUT_US);
    if (!EFI_ERROR(Status)) {
        mCurrentConn->BodyBytesRead += Got;
        *BytesRead = Got;
    }
    return Status;
}

UINTN EFIAPI HttpServerGetActiveConnections(VOID) {
    UINTN Count = 0;
    for (UINTN i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
        if (mConns[i].Active) Count++;
    }
    return Count;
}
