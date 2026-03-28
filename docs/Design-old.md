# HttpFS Design Document

## Overview

HttpFS is a lightweight UEFI Shell application that runs an HTTP file
server, enabling bidirectional file transfer between a remote workstation
and a UEFI host over the network. It exposes all UEFI filesystem volumes
(FS0:, FS1:, etc.) via a simple REST API accessible with `curl`, a browser,
or any HTTP client.

**Goal:** Minimal footprint, no TLS, no auth, no modules — just serve files.
This is a diagnostic/engineering tool, not a production service.

## Use Cases

1. **Upload EFI tools to a server in UEFI Shell** — push IpmiTool.efi,
   diagnostics, drivers, etc. without virtual media or USB.
2. **Download logs/data from UEFI** — pull UEFI Shell output, crash dumps,
   SMBIOS tables, or any file from the EFI System Partition.
3. **Remote directory browsing** — list filesystem contents from a browser
   to understand what's on the target.
4. **ARM64 server bootstrapping** — transfer files to ARM64 servers
   where BMC virtual media is unreliable.
5. **Recursive folder transfer** — upload or download entire directory trees
   for bulk deployment or retrieval.

## Architecture (SOC + DRY)

Strict separation of concerns — three layers with shared libraries:

```
┌─────────────────────────────────────────────────────────┐
│  Application Layer                                      │
│  ┌────────────────────────────────────────────────────┐ │
│  │ Application/HttpFS/Main.c                        │ │
│  │   CLI parsing, mode dispatch, progress display,    │ │
│  │   main poll loop, ESC handling                     │ │
│  └──────────┬──────────────────────┬──────────────────┘ │
│             │                      │                     │
│  Library Layer (reusable by future WebDAV, other tools) │
│  ┌──────────▼──────┐  ┌───────────▼────────────────┐   │
│  │ HttpServerLib    │  │ FileTransferLib             │   │
│  │                  │  │                             │   │
│  │ Listen/Accept    │  │ Volume enumeration          │   │
│  │ HTTP parsing     │  │ Stream read/write (8 KB)    │   │
│  │ Route dispatch   │  │ Directory listing           │   │
│  │ Response framing │  │ Recursive walk              │   │
│  │ Connection pool  │  │ SHA-256 integrity           │   │
│  │ Range headers    │  │ Progress tracking callback  │   │
│  └──────────┬──────┘  └───────────┬────────────────┘   │
│             │                      │                     │
│  ┌──────────▼──────────────────────▼────────────────┐   │
│  │ NetworkLib                                        │   │
│  │   NIC discovery, DHCP, static IP, IP4Config2      │   │
│  └──────────┬──────────────────────┬────────────────┘   │
│             │                      │                     │
│  Protocol Layer (UEFI firmware)                         │
│  ┌──────────▼──────┐  ┌───────────▼────────────────┐   │
│  │ EFI_TCP4         │  │ EFI_SIMPLE_FILE_SYSTEM     │   │
│  │ EFI_IP4_CONFIG2  │  │ EFI_FILE_PROTOCOL          │   │
│  │ EFI_DHCP4        │  │                             │   │
│  │ EFI_SNP          │  │                             │   │
│  └─────────────────┘  └────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Layer Rules

1. **Main.c** (Application) — parse args, dispatch mode, display progress,
   poll loop. Only calls HttpServerLib and FileTransferLib.
2. **HttpServerLib** — TCP connection management, HTTP request parsing,
   route dispatch, response framing, streaming support. Only calls
   NetworkLib for initialization.
3. **FileTransferLib** — volume enumeration, streaming file read/write,
   directory listing (HTML + JSON), recursive walk, SHA-256, delete,
   mkdir. Only calls UEFI file system protocols.
4. **NetworkLib** — NIC discovery, DHCP configuration, static IP, address
   detection. Only calls UEFI network protocols.

Libraries are independent of each other (HttpServerLib does not call
FileTransferLib and vice versa). Main.c wires them together via route
handler callbacks.

## REST API

All paths use the format `/<volume>/<path>`. Volume names match UEFI Shell
conventions: `fs0`, `fs1`, etc. Trailing slash on directories is optional.

### Endpoints

| Method   | Path                  | Description                      |
|----------|-----------------------|----------------------------------|
| `GET /`  |                       | List all volumes (JSON/HTML)     |
| `GET`    | `/<vol>/<path>`       | Download file or list directory  |
| `PUT`    | `/<vol>/<path>`       | Upload (create/overwrite) file   |
| `DELETE` | `/<vol>/<path>`       | Delete file or empty directory   |
| `POST`   | `/<vol>/<path>?mkdir` | Create directory (with parents)  |

### Headers

| Header              | Direction | Description                         |
|---------------------|-----------|-------------------------------------|
| `Content-Length`    | Both      | File size (enables progress bar)    |
| `X-SHA256`         | Response  | SHA-256 hash of uploaded file       |
| `Range`            | Request   | Byte range for resume (`bytes=N-`)  |
| `Content-Range`    | Response  | Byte range in partial response      |
| `Accept`           | Request   | `application/json` for JSON listing |

### Response Formats

- **File download**: Raw file bytes with `Content-Length` and
  `Content-Type: application/octet-stream`.
- **Directory listing**: HTML by default (browsable in browser), JSON if
  `Accept: application/json` header is present.
- **Upload response**: `201 Created` with `X-SHA256` integrity hash.
- **Resume**: `206 Partial Content` with `Content-Range` header.
- **Errors**: Plain text body with HTTP status code.

### Examples

```bash
# List volumes
curl http://192.168.1.100:8080/

# Browse directory
curl http://192.168.1.100:8080/fs0/EFI/Boot/

# Download file
curl http://192.168.1.100:8080/fs0/EFI/Boot/bootaa64.efi -o boot.efi

# Upload file
curl -T IpmiTool.efi http://192.168.1.100:8080/fs0/IpmiTool.efi

# Upload multiple files (client-side loop)
for f in *.efi; do curl -T "$f" http://192.168.1.100:8080/fs0/tools/$f; done

# Recursive download (client-side script)
xfer.sh download http://192.168.1.100:8080/fs0/EFI/ ./local-copy/

# Delete file
curl -X DELETE http://192.168.1.100:8080/fs0/temp/old.log

# Create directory
curl -X POST "http://192.168.1.100:8080/fs0/tools/?mkdir"

# Resume interrupted download
curl -C - http://192.168.1.100:8080/fs0/large.bin -o large.bin
```

## CLI Interface

```
HttpFS.efi [serve] [path] [options]

Commands:
  serve [path]     Start HTTP file server (default command)
                   If path given, restrict access to that directory

Options:
  -p <port>        HTTP listen port (default: 8080)
  -i <ip>          Static IP address (default: DHCP)
  -n <index>       Bind to NIC index (default: first with IP)
  -t <seconds>     Auto-exit after idle timeout (0 = disabled)
  --read-only      Only allow downloads (GET), block uploads/deletes
  --write-only     Only allow uploads (PUT), block downloads
  -v               Verbose logging (show request details)
  -h               Show help

Press ESC to stop the server.
```

### Startup Output

```
HttpFS v1.0 — UEFI HTTP File Server
Listening on 192.168.1.100:8080
Mode: read-write
Volumes:
  fs0: PciRoot(0x0)/Pci(0x1F,0x2)/Sata(0x0,...)/HD(1,GPT,...)
  fs1: VenHw(...)/HD(1,GPT,...)
Press ESC to stop.
```

### Transfer Progress Display

During active transfers, show scp-style progress on the UEFI console:

```
PUT  IpmiTool.efi      [============>        ]  62%  1.2 MB/s   3.4 MB /  5.5 MB
GET  bootaa64.efi       [====================] 100%  2.8 MB/s  12.0 MB / 12.0 MB  done
PUT  large-image.bin    [=>                   ]   8%  956 KB/s  42.0 MB / 512 MB
```

When `Content-Length` is unknown (chunked upload), show bytes only:

```
PUT  unknown.dat        [<=>                  ]  ---   1.1 MB/s   7.2 MB
```

Progress is updated in-place (carriage return, no newline) to avoid
scrolling. Completed transfers print a final line with `done`.

## Project Layout

```
docs/
  Design.md                          This document
scripts/
  build.sh                           EDK2 build wrapper (X64 + AARCH64)
  qemu.sh                            QEMU launcher with port forwarding
  test.sh                            curl-based integration tests
  xfer.sh                            Client-side helper for recursive xfer
HttpFsPkg/
  HttpFsPkg.dec                    Package declaration
  HttpFsPkg.dsc                    Build descriptor
  Include/Library/
    HttpServerLib.h                  HTTP server public API
    FileTransferLib.h                File transfer public API
    NetworkLib.h                     Network init public API
  Library/
    HttpServerLib/
      HttpServerLib.inf              Build file
      HttpServer.c                   TCP/HTTP server implementation
      HttpServerInternal.h           Internal types (connection pool)
    FileTransferLib/
      FileTransferLib.inf            Build file
      FileTransfer.c                 File I/O, streaming, SHA-256
      DirList.c                      Directory listing (HTML + JSON)
    NetworkLib/
      NetworkLib.inf                 Build file
      Network.c                      NIC discovery, DHCP, static IP
  Application/HttpFS/
    HttpFS.inf                     Module build file
    Main.c                           Entry point, CLI, poll loop, progress
```

## UEFI Protocol Dependencies

| Protocol                          | Library        | Usage                     |
|-----------------------------------|----------------|---------------------------|
| `EFI_TCP4_SERVICE_BINDING`        | HttpServerLib  | Create TCP child handles  |
| `EFI_TCP4_PROTOCOL`              | HttpServerLib  | Listen, accept, send, recv|
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`| FileTransferLib| Open volumes, file I/O    |
| `EFI_FILE_PROTOCOL`              | FileTransferLib| Read, write, get info     |
| `EFI_IP4_CONFIG2_PROTOCOL`       | NetworkLib     | Check/configure IP        |
| `EFI_DHCP4_PROTOCOL`            | NetworkLib     | DHCP fallback             |
| `EFI_SIMPLE_NETWORK_PROTOCOL`   | NetworkLib     | NIC detection             |

## Network Initialization

Pattern from SoftBMC (implemented in NetworkLib):

1. Enumerate all handles with `EFI_SIMPLE_NETWORK_PROTOCOL`
2. If `-n <index>` specified, filter to that NIC
3. If `-i <ip>` specified, configure static IP on selected NIC
4. Otherwise, check `EFI_IP4_CONFIG2_PROTOCOL` for existing IP config
5. If unconfigured, set DHCP policy and wait (up to 10s) for lease
6. Fallback to `EFI_DHCP4_PROTOCOL` direct if IP4Config2 unavailable
7. Print IP address and start listening

If no NIC has an IP after configuration, print error and exit.

## Implementation Plan

### Phase 1: Libraries + Volume Listing

Build the three libraries and a minimal application that starts the server
and serves volume/directory listings.

**NetworkLib:**
- NIC enumeration and selection
- DHCP configuration with IP4Config2 and DHCP4 fallback
- Static IP configuration
- `NetworkInit()`, `NetworkGetAddress()`, `NetworkCleanup()`

**HttpServerLib:**
- TCP4 listener on configurable port
- Connection pool (4 slots) with cooperative polling
- HTTP/1.1 request parsing (method, path, headers)
- Route table with method matching and permission flags
- Response API: `HttpSendResponse()`, `HttpSendHeaders()`, `HttpSendChunk()`
- `HttpServerInit()`, `HttpServerPoll()`, `HttpServerStop()`

**FileTransferLib:**
- Volume enumeration (all `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` handles)
- Directory listing as HTML and JSON
- `FileTransferInit()`, `FileTransferListVolumes()`,
  `FileTransferListDir()`

**Application:**
- Arg parsing (port, IP, NIC, mode, verbose)
- Wire route handlers to library calls
- Main poll loop with ESC detection
- `GET /` and `GET /<vol>/<path>/` routes

**Build files:**
- `.dec`, `.dsc`, `.inf` for package and all libraries
- `scripts/build.sh` for X64 and AARCH64

**Delivers:** Browsable directory listing from a browser.

### Phase 2: File Transfer + Progress

**FileTransferLib additions:**
- Streaming file read in 8 KB chunks with progress callback
- Streaming file write in 8 KB chunks with progress callback
- SHA-256 computation during upload (inline, no second pass)
- Resume support: seek to offset on Range header, respond 206

**HttpServerLib additions:**
- PUT body accumulation with streaming to callback
- Range header parsing
- Content-Range response header

**Application additions:**
- Progress display (scp-style bar with speed, size, percentage)
- `GET /<vol>/<path>` file download route
- `PUT /<vol>/<path>` file upload route
- `--read-only` / `--write-only` enforcement

**Delivers:** Full bidirectional file transfer with progress display.

### Phase 3: Delete, Mkdir, Client Script, Polish

**FileTransferLib additions:**
- File and empty directory deletion
- Directory creation (with parent creation)
- Recursive directory walk (for JSON listing with `?recursive`)

**Application additions:**
- `DELETE /<vol>/<path>` route
- `POST /<vol>/<path>?mkdir` route
- `-t` idle timeout auto-exit
- Verbose request logging (`-v`)

**Client-side script:**
- `scripts/xfer.sh` — bash wrapper around curl for recursive
  upload/download of directory trees. Walks JSON directory listing
  to discover files, transfers each one.

**Delivers:** Complete feature set.

### Phase 4: Build + Test

- `scripts/qemu.sh` — QEMU launcher with port forwarding
  (`hostfwd=tcp::8080-:8080`), boots HttpFS.efi via startup.nsh
- `scripts/test.sh` — automated curl-based integration tests
- Test both X64 and AARCH64 in QEMU

**Delivers:** Automated test suite, both architectures verified.

## Design Decisions

### No TLS
This is a diagnostic tool on isolated management networks. TLS adds mbedTLS
dependency (~200 KB), certificate management complexity, and build time.
Not worth it for the use case.

### No Authentication
Same rationale. The tool runs interactively under the UEFI Shell — the
operator is physically or virtually present. If security is needed, run on
an isolated management VLAN.

### Server Mode Only (No One-Shot)
The server runs until the user presses ESC. No automatic exit after a
single transfer. This keeps the implementation simple and lets the user
transfer multiple files in one session. The optional `-t` idle timeout
provides automatic cleanup when needed.

### Streaming I/O
Files are transferred in 8 KB chunks, never fully buffered in RAM. This
supports unlimited file sizes and keeps memory footprint minimal. Progress
tracking hooks into the streaming callbacks.

### SHA-256 Integrity
Uploads compute SHA-256 as data arrives (single pass, no rewind) and
return it in the `X-SHA256` response header. The client can verify
integrity without additional round trips.

### Resume via Range Header
Downloads support the standard HTTP `Range: bytes=N-` header. If a large
transfer is interrupted, `curl -C -` resumes from where it left off.
Critical for multi-hundred-MB transfers over flaky management networks.

### Client-Side Recursive Transfer
The server handles one file per request. Recursive directory transfer is
handled by a client-side script (`xfer.sh`) that walks the JSON directory
listing and issues individual curl requests. This keeps the server simple
and avoids tar/archive parsing.

### Connection Pool
UEFI has no threads. The server uses a fixed-size connection pool
(4 connections) with cooperative polling. The main loop polls all
connections and the keyboard (ESC to quit). 4 connections is sufficient
for a single-user diagnostic tool.

### HTML Directory Listing
Default directory listing is HTML so it's browsable in a web browser.
JSON available via `Accept: application/json` for scripting and the
recursive transfer client. The HTML is minimal — no CSS, no JavaScript,
just a plain `<ul>` with links plus file sizes and dates.

### SOC Library Split
HttpServerLib, FileTransferLib, and NetworkLib are independent EDK2
libraries. This enables future consumers (WebDAV application, other
tools) to reuse the networking and file I/O without code duplication.

## Estimated Size

| Component              | Lines (approx) |
|------------------------|----------------|
| NetworkLib             | 250            |
| HttpServerLib          | 700            |
| FileTransferLib        | 500            |
| Main.c (application)   | 300            |
| Build files (.dec/dsc/inf) | 150        |
| scripts/               | 300            |
| **Total**              | **~2200**      |

## Testing Strategy

### QEMU Integration Tests

`scripts/qemu.sh` boots HttpFS.efi in QEMU with port forwarding
(`hostfwd=tcp::8080-:8080`). `scripts/test.sh` runs curl commands
against `localhost:8080` and validates responses:

1. `GET /` — verify volume list contains `fs0`
2. `GET /fs0/` — verify directory listing (HTML)
3. `GET /fs0/?json` or `Accept: application/json` — verify JSON listing
4. `PUT /fs0/test.txt` — upload file, check 201 + X-SHA256 header
5. `GET /fs0/test.txt` — download and compare SHA-256 to upload
6. `DELETE /fs0/test.txt` — delete, verify 200
7. `GET /fs0/test.txt` — verify 404
8. `POST /fs0/testdir/?mkdir` — create directory, verify 201
9. Large file transfer — upload/download 1 MB file, verify SHA-256
10. Resume test — partial download, then `Range: bytes=N-`, verify content
11. Read-only mode — start with `--read-only`, verify PUT returns 403
12. Recursive transfer — upload directory tree via `xfer.sh`, verify

Tests run on both X64 and AARCH64 QEMU targets.

### Manual Testing

On real ARM64 server hardware:
1. Boot to UEFI Shell
2. Run `HttpFS.efi -v`
3. From workstation: `curl -T IpmiTool.efi http://<server-ip>:8080/fs0/`
4. Verify progress display on UEFI console
5. In UEFI Shell: verify `ls fs0:\IpmiTool.efi` shows the file
6. Download: `curl http://<server-ip>:8080/fs0/EFI/ -o listing.html`
7. Browse in browser: `http://<server-ip>:8080/`

## Future Enhancements (Planned, Not in v1)

### WebDAV + Remote Command Execution

A future phase adds WebDAV protocol support (RFC 4918) to HttpServerLib,
enabling mounting of UEFI filesystems as network drives on
Windows/Linux/macOS:

```
# Windows
net use Z: \\192.168.1.100@8080\dav\fs0

# Linux
mount -t davfs http://192.168.1.100:8080/dav/fs0 /mnt/uefi

# macOS
mount_webdav http://192.168.1.100:8080/dav/fs0 /mnt/uefi
```

**Remote command execution** via a "command drop" mechanism:

- A companion DXE driver (`HttpFSCmdDxe`) watches a designated
  directory (e.g., `fs0:\xfer\cmd\`) for `.nsh` files.
- When a script appears (uploaded via WebDAV or HTTP PUT), the driver
  executes it via `EFI_SHELL_PROTOCOL->Execute()`.
- Output is captured to `fs0:\xfer\cmd\<script>.out`.
- The remote user uploads a command, then polls/downloads the output.

This requires:
- WebDAV protocol handler in HttpServerLib (~800 lines of XML/PROPFIND)
- HttpFSCmdDxe driver (filesystem watcher + shell execution)
- Security consideration: command execution should be opt-in (`--enable-cmd`)

The library split (HttpServerLib, FileTransferLib, NetworkLib) is designed
to support this without restructuring.

### Other Future Items

- IPv6 support
- Drag-and-drop web UI (minimal JavaScript upload form)
- Transfer queue display (multiple concurrent transfers)
- Bandwidth throttling
- File integrity verification on download (X-SHA256 response header)
