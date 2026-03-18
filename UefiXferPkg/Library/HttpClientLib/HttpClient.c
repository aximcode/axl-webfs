/** @file
  HttpClientLib — HTTP/1.1 client over TCP4.

  TCP connect/send/receive adapted from SoftBMC Core/TcpClient.c and
  Core/TcpUtil.c. HTTP framing is hand-rolled for the minimal subset
  needed by UefiXfer (talking to xfer-server.py).

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/HttpClientLib.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>

#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>

#define TCP_SEND_CHUNK_SIZE      (32 * 1024)
#define TCP_SEND_RETRY_US        (2 * 1000 * 1000)
#define TCP_SEND_COMPLETION_US   (5 * 1000 * 1000)
#define TCP_RECV_TIMEOUT_US      (10 * 1000 * 1000)
#define HTTP_REQUEST_BUF_SIZE    4096

static EFI_HANDLE mImageHandle = NULL;

// ============================================================================
// TCP helpers (adapted from SoftBMC TcpUtil.c / TcpClient.c)
// ============================================================================

/// Send a single chunk with retry on EFI_NOT_READY.
static EFI_STATUS TcpSendChunk(
    IN EFI_TCP4_PROTOCOL  *Tcp4,
    IN VOID               *Buffer,
    IN UINTN              Length,
    IN UINTN              RetryTimeoutUs,
    IN UINTN              CompletionTimeoutUs
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

    // Submit with retry on EFI_NOT_READY
    Status = Tcp4->Transmit(Tcp4, &TxToken);
    if (Status == EFI_NOT_READY && RetryTimeoutUs > 0) {
        UINTN Elapsed = 0;
        while (Elapsed < RetryTimeoutUs) {
            Tcp4->Poll(Tcp4);
            gBS->Stall(5000);
            Elapsed += 5000;
            Status = Tcp4->Transmit(Tcp4, &TxToken);
            if (Status != EFI_NOT_READY) break;
        }
    }

    if (EFI_ERROR(Status)) {
        gBS->CloseEvent(Event);
        return Status;
    }

    // Wait for completion
    UINTN Elapsed = 0;
    while (Elapsed < CompletionTimeoutUs) {
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

/// Send data in 32KB chunks.
static EFI_STATUS TcpSend(
    IN EFI_TCP4_PROTOCOL  *Tcp4,
    IN VOID               *Buffer,
    IN UINTN              Length
) {
    UINT8 *Ptr = (UINT8 *)Buffer;
    UINTN Remaining = Length;

    while (Remaining > 0) {
        UINTN ChunkLen = (Remaining > TCP_SEND_CHUNK_SIZE)
                       ? TCP_SEND_CHUNK_SIZE : Remaining;
        EFI_STATUS Status = TcpSendChunk(
            Tcp4, Ptr, ChunkLen, TCP_SEND_RETRY_US, TCP_SEND_COMPLETION_US);
        if (EFI_ERROR(Status)) return Status;
        Ptr += ChunkLen;
        Remaining -= ChunkLen;
    }

    return EFI_SUCCESS;
}

/// Receive up to MaxLen bytes with timeout.
static EFI_STATUS TcpReceive(
    IN  EFI_TCP4_PROTOCOL  *Tcp4,
    OUT UINT8              *Buffer,
    IN  UINTN              MaxLen,
    OUT UINTN              *ReceivedLen,
    IN  UINTN              TimeoutUs
) {
    *ReceivedLen = 0;

    EFI_EVENT RxEvent = NULL;
    EFI_STATUS Status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &RxEvent);
    if (EFI_ERROR(Status)) return Status;

    EFI_TCP4_RECEIVE_DATA RxData;
    SetMem(&RxData, sizeof(RxData), 0);
    RxData.UrgentFlag = FALSE;
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
    if (EFI_ERROR(Status)) {
        gBS->CloseEvent(RxEvent);
        return Status;
    }

    UINTN Elapsed = 0;
    while (Elapsed < TimeoutUs) {
        Tcp4->Poll(Tcp4);
        if (gBS->CheckEvent(RxEvent) == EFI_SUCCESS) {
            Status = RxToken.CompletionToken.Status;
            if (!EFI_ERROR(Status)) {
                *ReceivedLen = RxData.DataLength;
            }
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

// ============================================================================
// Internal: fill receive buffer
// ============================================================================

/// Read more data into Client->RecvBuf, appending after existing data.
static EFI_STATUS FillRecvBuf(IN HTTP_CLIENT *Client) {
    UINTN Space = sizeof(Client->RecvBuf) - Client->RecvBufLen;
    if (Space == 0) return EFI_BUFFER_TOO_SMALL;

    UINTN Got = 0;
    EFI_STATUS Status = TcpReceive(
        Client->Tcp4,
        (UINT8 *)Client->RecvBuf + Client->RecvBufLen,
        Space, &Got, TCP_RECV_TIMEOUT_US);
    if (!EFI_ERROR(Status)) {
        Client->RecvBufLen += Got;
    }
    return Status;
}

/// Compact consumed bytes from RecvBuf.
static VOID CompactRecvBuf(IN HTTP_CLIENT *Client) {
    if (Client->RecvBufPos == 0) return;
    UINTN Remaining = Client->RecvBufLen - Client->RecvBufPos;
    if (Remaining > 0) {
        CopyMem(Client->RecvBuf, Client->RecvBuf + Client->RecvBufPos, Remaining);
    }
    Client->RecvBufLen = Remaining;
    Client->RecvBufPos = 0;
}

// ============================================================================
// Internal: HTTP parsing helpers
// ============================================================================

/// Case-insensitive ASCII comparison.
static BOOLEAN HttpAsciiCmpI(CONST CHAR8 *A, CONST CHAR8 *B, UINTN Len) {
    for (UINTN i = 0; i < Len; i++) {
        CHAR8 Ca = A[i];
        CHAR8 Cb = B[i];
        if (Ca >= 'A' && Ca <= 'Z') Ca += 32;
        if (Cb >= 'A' && Cb <= 'Z') Cb += 32;
        if (Ca != Cb) return FALSE;
    }
    return TRUE;
}

/// Parse "HTTP/1.x NNN reason\r\n" from buffer. Returns bytes consumed.
static UINTN ParseStatusLine(
    IN  CONST CHAR8  *Buf,
    IN  UINTN        Len,
    OUT UINT16       *StatusCode
) {
    // Find end of status line
    for (UINTN i = 0; i + 1 < Len; i++) {
        if (Buf[i] == '\r' && Buf[i + 1] == '\n') {
            // Parse status code (skip "HTTP/1.x ")
            UINTN SpacePos = 0;
            for (UINTN j = 0; j < i; j++) {
                if (Buf[j] == ' ') { SpacePos = j; break; }
            }
            if (SpacePos > 0 && SpacePos + 4 <= i) {
                *StatusCode = (UINT16)(
                    (Buf[SpacePos + 1] - '0') * 100 +
                    (Buf[SpacePos + 2] - '0') * 10 +
                    (Buf[SpacePos + 3] - '0'));
            }
            return i + 2;
        }
    }
    return 0;  // Incomplete
}

/// Parse a single "Name: Value\r\n" header. Returns bytes consumed.
static UINTN ParseOneHeader(
    IN  CONST CHAR8  *Buf,
    IN  UINTN        Len,
    OUT CONST CHAR8  **Name,
    OUT UINTN        *NameLen,
    OUT CONST CHAR8  **Value,
    OUT UINTN        *ValueLen
) {
    // Empty line = end of headers
    if (Len >= 2 && Buf[0] == '\r' && Buf[1] == '\n') return 0;

    for (UINTN i = 0; i + 1 < Len; i++) {
        if (Buf[i] == '\r' && Buf[i + 1] == '\n') {
            // Find colon
            for (UINTN j = 0; j < i; j++) {
                if (Buf[j] == ':') {
                    *Name = Buf;
                    *NameLen = j;
                    // Skip ": " prefix on value
                    UINTN ValStart = j + 1;
                    while (ValStart < i && Buf[ValStart] == ' ') ValStart++;
                    *Value = Buf + ValStart;
                    *ValueLen = i - ValStart;
                    return i + 2;
                }
            }
            return i + 2;  // Malformed header, skip
        }
    }
    return 0;  // Incomplete
}

/// Parse decimal number from ASCII string.
static UINTN AsciiToUintn(CONST CHAR8 *Str, UINTN Len) {
    UINTN Val = 0;
    for (UINTN i = 0; i < Len; i++) {
        if (Str[i] < '0' || Str[i] > '9') break;
        Val = Val * 10 + (Str[i] - '0');
    }
    return Val;
}

// ============================================================================
// Public API
// ============================================================================

EFI_STATUS
EFIAPI
HttpClientConnect (
    IN  EFI_HANDLE        ImageHandle,
    IN  EFI_HANDLE        SbHandle,
    IN  EFI_IPv4_ADDRESS  ServerAddr,
    IN  UINT16            Port,
    IN  UINTN             TimeoutMs,
    OUT HTTP_CLIENT       *Client
    )
{
    if (Client == NULL) return EFI_INVALID_PARAMETER;

    mImageHandle = ImageHandle;
    SetMem(Client, sizeof(*Client), 0);
    CopyMem(&Client->ServerAddr, &ServerAddr, sizeof(EFI_IPv4_ADDRESS));
    Client->ServerPort = Port;
    Client->SbHandle = SbHandle;

    // Open TCP4 ServiceBinding
    EFI_STATUS Status = gBS->OpenProtocol(
        SbHandle, &gEfiTcp4ServiceBindingProtocolGuid,
        (VOID **)&Client->TcpSb,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) return Status;

    // Create TCP4 child
    Status = Client->TcpSb->CreateChild(Client->TcpSb, &Client->TcpChildHandle);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->OpenProtocol(
        Client->TcpChildHandle, &gEfiTcp4ProtocolGuid,
        (VOID **)&Client->Tcp4,
        ImageHandle, Client->TcpChildHandle,
        EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        Client->TcpSb->DestroyChild(Client->TcpSb, Client->TcpChildHandle);
        Client->TcpChildHandle = NULL;
        return Status;
    }

    // Configure as active (client) connection
    EFI_TCP4_ACCESS_POINT AccessPoint;
    SetMem(&AccessPoint, sizeof(AccessPoint), 0);
    AccessPoint.ActiveFlag = TRUE;
    AccessPoint.UseDefaultAddress = TRUE;
    AccessPoint.StationPort = 0;
    CopyMem(&AccessPoint.RemoteAddress, &ServerAddr, sizeof(EFI_IPv4_ADDRESS));
    AccessPoint.RemotePort = Port;

    EFI_TCP4_CONFIG_DATA TcpConfig;
    SetMem(&TcpConfig, sizeof(TcpConfig), 0);
    TcpConfig.TimeToLive = 64;
    TcpConfig.AccessPoint = AccessPoint;

    Status = Client->Tcp4->Configure(Client->Tcp4, &TcpConfig);
    if (EFI_ERROR(Status)) {
        HttpClientClose(Client);
        return Status;
    }

    // Connect with timeout
    EFI_EVENT ConnEvent = NULL;
    Status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &ConnEvent);
    if (EFI_ERROR(Status)) {
        HttpClientClose(Client);
        return Status;
    }

    EFI_TCP4_CONNECTION_TOKEN ConnToken;
    SetMem(&ConnToken, sizeof(ConnToken), 0);
    ConnToken.CompletionToken.Event = ConnEvent;
    ConnToken.CompletionToken.Status = EFI_ABORTED;

    Status = Client->Tcp4->Connect(Client->Tcp4, &ConnToken);
    if (EFI_ERROR(Status)) {
        gBS->CloseEvent(ConnEvent);
        HttpClientClose(Client);
        return Status;
    }

    UINTN TimeoutUs = TimeoutMs * 1000;
    UINTN Elapsed = 0;
    BOOLEAN Done = FALSE;

    while (Elapsed < TimeoutUs) {
        Client->Tcp4->Poll(Client->Tcp4);
        if (gBS->CheckEvent(ConnEvent) == EFI_SUCCESS) {
            Done = TRUE;
            break;
        }
        gBS->Stall(1000);
        Elapsed += 1000;
    }

    if (!Done) {
        Client->Tcp4->Cancel(Client->Tcp4, &ConnToken.CompletionToken);
        gBS->CloseEvent(ConnEvent);
        HttpClientClose(Client);
        return EFI_TIMEOUT;
    }

    Status = ConnToken.CompletionToken.Status;
    gBS->CloseEvent(ConnEvent);

    if (EFI_ERROR(Status)) {
        HttpClientClose(Client);
        return Status;
    }

    Client->Connected = TRUE;
    return EFI_SUCCESS;
}

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
    )
{
    if (Client == NULL || !Client->Connected || Method == NULL || Path == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    SetMem(Response, sizeof(*Response), 0);
    Response->ContentLength = MAX_UINTN;

    // Build request into a stack buffer
    CHAR8 ReqBuf[HTTP_REQUEST_BUF_SIZE];
    UINTN ReqLen = AsciiSPrint(
        ReqBuf, sizeof(ReqBuf),
        "%a %a HTTP/1.1\r\nHost: %d.%d.%d.%d:%d\r\nConnection: keep-alive\r\n",
        Method, Path,
        Client->ServerAddr.Addr[0], Client->ServerAddr.Addr[1],
        Client->ServerAddr.Addr[2], Client->ServerAddr.Addr[3],
        Client->ServerPort);

    // Add Content-Length for bodies
    if (Body != NULL && BodyLen > 0) {
        ReqLen += AsciiSPrint(
            ReqBuf + ReqLen, sizeof(ReqBuf) - ReqLen,
            "Content-Length: %d\r\n", BodyLen);
    }

    // Add caller-provided headers
    for (UINTN i = 0; i < HeaderCount && Headers != NULL; i++) {
        ReqLen += AsciiSPrint(
            ReqBuf + ReqLen, sizeof(ReqBuf) - ReqLen,
            "%a: %a\r\n", Headers[i].Name, Headers[i].Value);
    }

    // End headers
    ReqLen += AsciiSPrint(ReqBuf + ReqLen, sizeof(ReqBuf) - ReqLen, "\r\n");

    // Send request headers
    EFI_STATUS Status = TcpSend(Client->Tcp4, ReqBuf, ReqLen);
    if (EFI_ERROR(Status)) {
        Client->Connected = FALSE;
        return Status;
    }

    // Send body if present
    if (Body != NULL && BodyLen > 0) {
        Status = TcpSend(Client->Tcp4, (VOID *)Body, BodyLen);
        if (EFI_ERROR(Status)) {
            Client->Connected = FALSE;
            return Status;
        }
    }

    // Compact any leftover data in receive buffer
    CompactRecvBuf(Client);

    // Read response headers — accumulate data until we see \r\n\r\n
    CHAR8 *HeaderEnd = NULL;

    while (HeaderEnd == NULL) {
        Status = FillRecvBuf(Client);
        if (EFI_ERROR(Status)) {
            Client->Connected = FALSE;
            return (Status == EFI_CONNECTION_FIN ||
                    Status == EFI_CONNECTION_RESET)
                   ? EFI_CONNECTION_RESET : Status;
        }

        // Search for \r\n\r\n
        for (UINTN i = Client->RecvBufPos; i + 3 < Client->RecvBufLen; i++) {
            if (Client->RecvBuf[i] == '\r' && Client->RecvBuf[i + 1] == '\n' &&
                Client->RecvBuf[i + 2] == '\r' && Client->RecvBuf[i + 3] == '\n') {
                HeaderEnd = &Client->RecvBuf[i + 4];
                break;
            }
        }
    }

    // Copy raw headers into Response->HeaderBuf for stable pointers
    UINTN HeaderBytes = (UINTN)(HeaderEnd - Client->RecvBuf);
    if (HeaderBytes > sizeof(Response->HeaderBuf)) {
        HeaderBytes = sizeof(Response->HeaderBuf);
    }
    CopyMem(Response->HeaderBuf, Client->RecvBuf, HeaderBytes);

    // Parse status line
    UINTN Consumed = ParseStatusLine(
        Response->HeaderBuf, HeaderBytes, &Response->StatusCode);

    // Parse headers
    Response->HeaderCount = 0;
    while (Consumed < HeaderBytes && Response->HeaderCount < HTTP_MAX_HEADERS) {
        CONST CHAR8 *Name, *Value;
        UINTN NameLen, ValueLen;

        UINTN Ate = ParseOneHeader(
            Response->HeaderBuf + Consumed, HeaderBytes - Consumed,
            &Name, &NameLen, &Value, &ValueLen);
        if (Ate == 0) break;  // End of headers or incomplete

        // Null-terminate name and value in-place
        HTTP_HEADER *H = &Response->Headers[Response->HeaderCount];
        ((CHAR8 *)Name)[NameLen] = '\0';
        ((CHAR8 *)Value)[ValueLen] = '\0';
        H->Name = Name;
        H->Value = Value;

        // Detect Content-Length and Transfer-Encoding
        if (HttpAsciiCmpI(Name, "content-length", NameLen)) {
            Response->ContentLength = AsciiToUintn(Value, ValueLen);
        } else if (HttpAsciiCmpI(Name, "transfer-encoding", NameLen)) {
            if (HttpAsciiCmpI(Value, "chunked", ValueLen)) {
                Response->Chunked = TRUE;
            }
        }

        Response->HeaderCount++;
        Consumed += Ate;
    }

    // Advance RecvBufPos past the header block
    Client->RecvBufPos = (UINTN)(HeaderEnd - Client->RecvBuf);
    Response->BodyBytesRead = 0;

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
HttpClientReadBody (
    IN  HTTP_CLIENT        *Client,
    IN  HTTP_RESPONSE_CTX  *Response,
    OUT VOID               *Buffer,
    IN  UINTN              BufferSize,
    OUT UINTN              *BytesRead
    )
{
    if (Client == NULL || Response == NULL || Buffer == NULL || BytesRead == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    *BytesRead = 0;

    // Check if body is complete
    if (Response->ContentLength != MAX_UINTN &&
        Response->BodyBytesRead >= Response->ContentLength) {
        return EFI_SUCCESS;  // EOF
    }

    // Calculate how many body bytes we can serve from RecvBuf
    UINTN Available = Client->RecvBufLen - Client->RecvBufPos;
    if (Available > 0) {
        UINTN ToCopy = Available;
        if (ToCopy > BufferSize) ToCopy = BufferSize;
        if (Response->ContentLength != MAX_UINTN) {
            UINTN Remaining = Response->ContentLength - Response->BodyBytesRead;
            if (ToCopy > Remaining) ToCopy = Remaining;
        }
        CopyMem(Buffer, Client->RecvBuf + Client->RecvBufPos, ToCopy);
        Client->RecvBufPos += ToCopy;
        Response->BodyBytesRead += ToCopy;
        *BytesRead = ToCopy;

        if (Client->RecvBufPos >= Client->RecvBufLen) {
            CompactRecvBuf(Client);
        }
        return EFI_SUCCESS;
    }

    // Need more data from TCP
    CompactRecvBuf(Client);

    UINTN Want = BufferSize;
    if (Response->ContentLength != MAX_UINTN) {
        UINTN Remaining = Response->ContentLength - Response->BodyBytesRead;
        if (Want > Remaining) Want = Remaining;
    }
    if (Want == 0) return EFI_SUCCESS;

    // Read directly into caller's buffer to avoid extra copy
    UINTN Got = 0;
    EFI_STATUS Status = TcpReceive(
        Client->Tcp4, (UINT8 *)Buffer, Want, &Got, TCP_RECV_TIMEOUT_US);
    if (EFI_ERROR(Status)) {
        if (Status == EFI_CONNECTION_FIN) {
            // Server closed — treat as EOF
            return EFI_SUCCESS;
        }
        Client->Connected = FALSE;
        return Status;
    }

    Response->BodyBytesRead += Got;
    *BytesRead = Got;
    return EFI_SUCCESS;
}

CONST CHAR8 *
EFIAPI
HttpClientGetHeader (
    IN CONST HTTP_RESPONSE_CTX  *Response,
    IN CONST CHAR8              *Name
    )
{
    if (Response == NULL || Name == NULL) return NULL;

    UINTN NameLen = AsciiStrLen(Name);
    for (UINTN i = 0; i < Response->HeaderCount; i++) {
        if (HttpAsciiCmpI(Response->Headers[i].Name, Name, NameLen) &&
            Response->Headers[i].Name[NameLen] == '\0') {
            return Response->Headers[i].Value;
        }
    }
    return NULL;
}

VOID
EFIAPI
HttpClientClose (
    IN HTTP_CLIENT  *Client
    )
{
    if (Client == NULL) return;

    if (Client->Tcp4 != NULL) {
        // Try graceful close
        EFI_EVENT CloseEvent = NULL;
        if (!EFI_ERROR(gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &CloseEvent))) {
            EFI_TCP4_CLOSE_TOKEN CloseToken;
            SetMem(&CloseToken, sizeof(CloseToken), 0);
            CloseToken.CompletionToken.Event = CloseEvent;
            CloseToken.AbortOnClose = FALSE;

            EFI_STATUS Status = Client->Tcp4->Close(Client->Tcp4, &CloseToken);
            if (!EFI_ERROR(Status)) {
                for (UINTN i = 0; i < 500; i++) {
                    Client->Tcp4->Poll(Client->Tcp4);
                    if (gBS->CheckEvent(CloseEvent) == EFI_SUCCESS) break;
                    gBS->Stall(1000);
                }
            }
            gBS->CloseEvent(CloseEvent);
        }

        Client->Tcp4->Configure(Client->Tcp4, NULL);
        gBS->CloseProtocol(Client->TcpChildHandle, &gEfiTcp4ProtocolGuid,
                           mImageHandle, Client->TcpChildHandle);
    }

    if (Client->TcpSb != NULL && Client->TcpChildHandle != NULL) {
        Client->TcpSb->DestroyChild(Client->TcpSb, Client->TcpChildHandle);
    }

    SetMem(Client, sizeof(*Client), 0);
}
