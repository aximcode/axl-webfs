# HttpFS Design Document

> **Note:** This document was written for the original EDK2-based build.
> HttpFS has been ported to the AXL SDK (axl-cc). References to EDK2,
> .inf/.dsc/.dec files, JsonLib, UdkLib, and the HttpFsPkg directory
> structure are historical. See CLAUDE.md for the current project layout
> and build instructions.

## Overview

HttpFS is a UEFI toolkit for bidirectional file transfer and remote
filesystem access between a workstation and a UEFI host. It provides
two core capabilities:

1. **`mount`** — Mount a remote workstation directory as a UEFI volume
   (FSn:), enabling the UEFI Shell to directly access, execute, and
   modify files on the workstation in real time.

2. **`serve`** — Run an HTTP + WebDAV file server on the UEFI host,
   exposing local UEFI volumes to remote clients via `curl`, browser,
   or mounted as a network drive.

**Goal:** Eliminate file transfer friction in UEFI development and
diagnostics. Build on your workstation, run immediately in the UEFI Shell.

## Use Cases

### Mount (workstation → UEFI)

1. **Live development workflow** — mount a build output directory, run
   freshly compiled `.efi` files without any manual transfer step.
2. **ARM64 server bootstrapping** — mount tools directory on ARM64
   servers where BMC virtual media is unreliable.
3. **Bulk tool deployment** — mount a shared tools repository, run
   diagnostics directly from it.
4. **Live file editing** — edit files on the workstation, changes are
   instantly visible in the UEFI Shell.

### Serve (UEFI → workstation)

5. **Upload EFI tools to a server** — push IpmiTool.efi, diagnostics,
   drivers, etc. via `curl` without virtual media or USB.
6. **Download logs/data from UEFI** — pull crash dumps, SMBIOS tables,
   or any file from the EFI System Partition.
7. **Remote directory browsing** — list filesystem contents from a browser.
8. **Mount UEFI filesystem on workstation** — WebDAV serve mode lets the
   workstation mount the UEFI filesystem as a network drive.
9. **Recursive folder transfer** — upload or download entire directory
   trees for bulk deployment or retrieval.

## Architecture (SOC + DRY)

Strict separation of concerns with shared libraries:

```
┌─────────────────────────────────────────────────────────────────┐
│  Application / Driver Layer                                     │
│                                                                 │
│  ┌──────────────────────┐  ┌─────────────────────────────────┐ │
│  │ Application/HttpFS  │  │ Driver/WebDavFsDxe              │ │
│  │                       │  │                                 │ │
│  │ CLI: serve, mount,    │  │ DXE driver (stays resident)     │ │
│  │      umount           │  │ Installs EFI_SIMPLE_FILE_SYSTEM │ │
│  │ Progress display      │  │ Maps remote HTTP → file ops     │ │
│  │ Main poll loop (serve)│  │ Read-ahead cache                │ │
│  │ Driver loader (mount) │  │                                 │ │
│  └───────┬───────┬───────┘  └──────┬──────────────────────────┘ │
│          │       │                 │                             │
│  Library Layer (shared, reusable)                               │
│  ┌───────▼───┐ ┌▼────────────┐ ┌──▼──────────┐ ┌────────────┐ │
│  │HttpServer │ │HttpClientLib│ │FileTransfer │ │ JsonLib    │ │
│  │Lib        │ │             │ │Lib          │ │            │ │
│  │           │ │ Connect     │ │             │ │ Parse      │ │
│  │ Listen    │ │ GET/PUT/    │ │ Volume enum │ │ Extract    │ │
│  │ Accept    │ │ DELETE      │ │ Stream R/W  │ │ (minimal)  │ │
│  │ Parse req │ │ PROPFIND    │ │ Dir listing │ │            │ │
│  │ Route     │ │ Headers     │ │ SHA-256     │ │            │ │
│  │ Respond   │ │ Streaming   │ │ Recursive   │ │            │ │
│  │ WebDAV    │ │ Resume      │ │ walk        │ │            │ │
│  │ methods   │ │             │ │ Progress CB │ │            │ │
│  └─────┬─────┘ └──────┬─────┘ └──────┬──────┘ └─────┬──────┘ │
│        │               │              │               │        │
│  ┌─────▼───────────────▼──────────────▼───────────────▼──────┐ │
│  │ NetworkLib                                                 │ │
│  │   NIC discovery, DHCP, static IP, IP4Config2               │ │
│  └─────┬──────────────────────────────────────────────┬──────┘ │
│        │                                              │        │
│  Protocol Layer (UEFI firmware)                                │
│  ┌─────▼──────────────┐  ┌────────────────────────────▼──────┐ │
│  │ EFI_TCP4            │  │ EFI_SIMPLE_FILE_SYSTEM            │ │
│  │ EFI_IP4_CONFIG2     │  │ EFI_FILE_PROTOCOL                 │ │
│  │ EFI_DHCP4           │  │                                   │ │
│  │ EFI_SNP             │  │                                   │ │
│  └────────────────────┘  └───────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Layer Rules

1. **HttpFS.efi** (Application) — CLI parsing, dispatch `serve`/
   `mount`/`umount` commands. For `serve`: progress display, poll loop,
   ESC handling. For `mount`: load WebDavFsDxe driver, pass URL, exit.
2. **WebDavFsDxe.efi** (DXE Driver) — stays resident after load.
   Implements `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` backed by HttpClientLib.
   Only calls HttpClientLib, JsonLib, and NetworkLib.
3. **HttpServerLib** — TCP server, HTTP/1.1 request parsing, route
   dispatch, response framing, WebDAV method handling (PROPFIND,
   MKCOL, MOVE, COPY). Only calls NetworkLib for init.
4. **HttpClientLib** — HTTP/1.1 client. Connect, send requests, parse
   responses, streaming body, Range header support. Only calls NetworkLib.
5. **FileTransferLib** — volume enumeration, streaming file read/write,
   directory listing (HTML, JSON, WebDAV XML), recursive walk, SHA-256.
   Only calls UEFI file system protocols.
6. **JsonLib** — minimal JSON parser for directory listing responses.
   Stateless, no allocations beyond caller-provided buffer.
7. **NetworkLib** — NIC discovery, DHCP, static IP, address detection.
   Only calls UEFI network protocols.

Libraries are independent of each other. Application/driver layers
wire them together.

## Mount Command

### How It Works

The `mount` command loads a companion DXE driver (`WebDavFsDxe.efi`)
that installs `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` on a new device handle.
The UEFI Shell sees this as a new volume (FSn:).

```
Workstation                              UEFI Host
┌──────────────────┐                    ┌──────────────────────┐
│ xfer-server.py   │  HTTP/JSON         │ WebDavFsDxe.efi      │
│                  │◄──────────────────►│                      │
│ Serves files     │  GET/PUT/DELETE    │ EFI_SIMPLE_FILE_     │
│ from a local dir │  + JSON dir list   │ SYSTEM_PROTOCOL      │
│                  │                    │                      │
│ ~/share/         │                    │ Appears as FS1:      │
│  ├── tools/      │                    │                      │
│  │   └── ipmi.efi│                    │ Shell> ls fs1:\      │
│  └── builds/     │                    │   tools/             │
│      └── diag.efi│                    │   builds/            │
└──────────────────┘                    └──────────────────────┘
```

### Protocol (v1 — Simple HTTP/JSON)

The workstation runs `xfer-server.py` (provided with HttpFS). The
protocol is plain HTTP with JSON directory listings — no WebDAV XML
parsing needed on the UEFI side:

```
GET  /files/<path>        → download file (with Range support)
PUT  /files/<path>        → upload/create file
DELETE /files/<path>      → delete file or empty directory
POST /files/<path>?mkdir  → create directory (with parents)
GET  /list/<path>         → JSON directory listing:
                            [{"name":"foo.efi","size":48384,"dir":false,
                              "modified":"2026-03-17T14:30:00Z"}, ...]
GET  /info                → server info: {"version":"1.0","root":"/share"}
```

### EFI_FILE_PROTOCOL Mapping

Every UEFI file operation in the driver translates to HTTP:

| EFI_FILE_PROTOCOL      | HTTP Request                        |
|-------------------------|-------------------------------------|
| `Open()`               | `GET /list/<path>` (check exists)   |
| `Close()`              | (no-op, flush cache)                |
| `Read()` (file)        | `GET /files/<path>` (Range for pos) |
| `Read()` (dir)         | `GET /list/<path>` (iterate entries) |
| `Write()`              | `PUT /files/<path>` (Range for pos) |
| `Delete()`             | `DELETE /files/<path>`              |
| `GetInfo(FILE_INFO)`   | `GET /list/<parent>` (find entry)   |
| `SetInfo(FILE_INFO)`   | (not supported in v1)               |
| `GetPosition()`        | (local tracking, no HTTP)           |
| `SetPosition()`        | (local tracking, no HTTP)           |
| `Flush()`              | (no-op in v1 — writes are immediate)|

### Caching Strategy

Without caching, every `ls` or file access hammers the network. The
driver implements a simple two-level cache:

1. **Directory cache** — cache `GET /list/<path>` results for 2 seconds.
   Invalidated on any write/delete operation to that directory.
   Covers the common case of `ls` followed by accessing listed files.

2. **Read-ahead buffer** — when reading a file, prefetch the next 64 KB
   into a buffer. Sequential reads (which is what `cp` and most tools
   do) hit the buffer instead of issuing per-8KB HTTP requests.

No write caching — writes go through immediately. This ensures the
workstation always has the latest data and avoids data loss.

### CLI

```
HttpFS.efi mount <url> [options]
HttpFS.efi umount <volume>

Mount options:
  -r               Mount read-only
  -c <seconds>     Directory cache TTL (default: 2)

Examples:
  HttpFS.efi mount http://10.0.0.5:8080/
  Mounting http://10.0.0.5:8080/ ...
  Loading WebDavFs driver... OK
  Mapped as FS1:

  Shell> ls fs1:\builds\
    IpmiTool.efi    48,384  03/17/2026
    HttpFS.efi    62,720  03/17/2026

  Shell> fs1:\builds\IpmiTool.efi mc info
    Device ID: 0x20 ...

  HttpFS.efi umount fs1:
  Unmounting FS1:... OK
```

### xfer-server.py (Workstation Side)

A self-contained Python script (~200 lines) that serves a local directory
over HTTP with the JSON listing API. No dependencies beyond Python stdlib:

```bash
# Serve current directory
./xfer-server.py

# Serve specific directory on specific port
./xfer-server.py --root /path/to/share --port 9090 --bind 0.0.0.0

# Read-only mode (no PUT/DELETE)
./xfer-server.py --read-only
```

Output:
```
xfer-server v1.0
Serving /home/user/share on 0.0.0.0:8080
Ready for HttpFS mount connections.
```

## Serve Command

### Modes

The `serve` command runs an HTTP + WebDAV file server on the UEFI host,
exposing local UEFI volumes to remote clients.

**HTTP mode** (default): REST API accessible with `curl` or a browser.

**WebDAV mode** (`--webdav`): Adds WebDAV methods (PROPFIND, MKCOL,
MOVE, COPY) so the remote workstation can mount the UEFI filesystem
as a network drive:

```bash
# Windows
net use Z: \\192.168.1.100@8080\fs0

# Linux
mount -t davfs http://192.168.1.100:8080/fs0 /mnt/uefi

# macOS
mount_webdav http://192.168.1.100:8080/fs0 /mnt/uefi
```

### REST API (HTTP Mode)

All paths use the format `/<volume>/<path>`. Volume names match UEFI Shell
conventions: `fs0`, `fs1`, etc.

| Method   | Path                  | Description                      |
|----------|-----------------------|----------------------------------|
| `GET /`  |                       | List all volumes (JSON/HTML)     |
| `GET`    | `/<vol>/<path>`       | Download file or list directory  |
| `PUT`    | `/<vol>/<path>`       | Upload (create/overwrite) file   |
| `DELETE` | `/<vol>/<path>`       | Delete file or empty directory   |
| `POST`   | `/<vol>/<path>?mkdir` | Create directory (with parents)  |

### WebDAV API (WebDAV Mode)

Extends HTTP mode with RFC 4918 methods:

| Method     | Path              | Description                      |
|------------|-------------------|----------------------------------|
| `PROPFIND` | `/<vol>/<path>`   | File/directory properties (XML)  |
| `MKCOL`    | `/<vol>/<path>`   | Create directory (collection)    |
| `MOVE`     | `/<vol>/<path>`   | Rename/move file or directory    |
| `COPY`     | `/<vol>/<path>`   | Copy file or directory           |
| `OPTIONS`  | `*`               | DAV compliance class             |
| `LOCK`     | `/<vol>/<path>`   | Lock (stub — always succeeds)    |
| `UNLOCK`   | `/<vol>/<path>`   | Unlock (stub)                    |

LOCK/UNLOCK are stubs that always succeed — required for Windows WebDAV
client compatibility but meaningless for a single-user tool.

### Headers

| Header              | Direction | Description                         |
|---------------------|-----------|-------------------------------------|
| `Content-Length`    | Both      | File size (enables progress bar)    |
| `X-SHA256`         | Response  | SHA-256 hash of uploaded file       |
| `Range`            | Request   | Byte range for resume (`bytes=N-`)  |
| `Content-Range`    | Response  | Byte range in partial response      |
| `Accept`           | Request   | `application/json` for JSON listing |
| `Depth`            | Request   | PROPFIND depth (0, 1, infinity)     |
| `Destination`      | Request   | Target URI for MOVE/COPY            |

### Response Formats

- **File download**: Raw bytes with `Content-Length` and `Content-Type`.
- **Directory listing (HTTP)**: HTML by default, JSON with
  `Accept: application/json`.
- **Directory listing (WebDAV)**: XML multistatus response per RFC 4918.
- **Upload response**: `201 Created` with `X-SHA256` integrity hash.
- **Resume**: `206 Partial Content` with `Content-Range` header.
- **Errors**: Plain text body with HTTP status code.

### Examples

```bash
# List volumes
curl http://192.168.1.100:8080/

# Download file
curl http://192.168.1.100:8080/fs0/EFI/Boot/bootaa64.efi -o boot.efi

# Upload file
curl -T IpmiTool.efi http://192.168.1.100:8080/fs0/IpmiTool.efi

# Upload multiple files
for f in *.efi; do curl -T "$f" http://192.168.1.100:8080/fs0/tools/$f; done

# Recursive download (client-side script)
xfer.sh download http://192.168.1.100:8080/fs0/EFI/ ./local-copy/

# Delete file
curl -X DELETE http://192.168.1.100:8080/fs0/temp/old.log

# Create directory
curl -X POST "http://192.168.1.100:8080/fs0/tools/?mkdir"

# Resume interrupted download
curl -C - http://192.168.1.100:8080/fs0/large.bin -o large.bin

# Mount on Windows (WebDAV mode)
net use Z: \\192.168.1.100@8080\fs0
```

### CLI

```
HttpFS.efi [serve] [path] [options]

Options:
  -p <port>        HTTP listen port (default: 8080)
  -i <ip>          Static IP address (default: DHCP)
  -n <index>       Bind to NIC index (default: first with IP)
  -t <seconds>     Auto-exit after idle timeout (0 = disabled)
  --read-only      Only allow downloads, block uploads/deletes
  --write-only     Only allow uploads, block downloads
  --webdav         Enable WebDAV methods (PROPFIND, MKCOL, etc.)
  -v               Verbose logging (show request details)
  -h               Show help

Press ESC to stop the server.
```

### Startup Output

```
HttpFS v1.0 — UEFI HTTP File Server
Listening on 192.168.1.100:8080
Mode: read-write (WebDAV enabled)
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
  Design.md                            This document
scripts/
  build.sh                             EDK2 build wrapper (X64 + AARCH64)
  qemu.sh                              QEMU launcher with port forwarding
  test.sh                              Automated integration tests
  xfer.sh                              Client-side recursive transfer helper
  xfer-server.py                       Workstation file server for mount
HttpFsPkg/
  HttpFsPkg.dec                      Package declaration
  HttpFsPkg.dsc                      Build descriptor
  Include/Library/
    NetworkLib.h                       Network init public API
    HttpClientLib.h                    HTTP client public API
    HttpServerLib.h                    HTTP/WebDAV server public API
    FileTransferLib.h                  File transfer public API
    JsonLib.h                          Minimal JSON parser API
  Library/
    NetworkLib/
      NetworkLib.inf                   Build file
      Network.c                        NIC discovery, DHCP, static IP
    HttpClientLib/
      HttpClientLib.inf                Build file
      HttpClient.c                     HTTP client: connect, request, stream
    HttpServerLib/
      HttpServerLib.inf                Build file
      HttpServer.c                     TCP/HTTP server, connection pool
      WebDav.c                         WebDAV method handlers + XML generation
      HttpServerInternal.h             Internal types
    FileTransferLib/
      FileTransferLib.inf              Build file
      FileTransfer.c                   File I/O, streaming, SHA-256
      DirList.c                        Directory listing (HTML, JSON, XML)
    JsonLib/
      JsonLib.inf                      Build file
      Json.c                           Minimal JSON tokenizer/extractor
  Driver/WebDavFsDxe/
    WebDavFsDxe.inf                    Build file
    WebDavFs.c                         Driver entry, protocol install
    WebDavFsFile.c                     EFI_FILE_PROTOCOL implementation
    WebDavFsCache.c                    Directory cache + read-ahead buffer
    WebDavFsInternal.h                 Internal types
  Application/HttpFS/
    HttpFS.inf                       Module build file
    Main.c                             Entry point, CLI dispatch
    CmdServe.c                         serve command: poll loop, progress
    CmdMount.c                         mount/umount: driver load/unload
```

## UEFI Protocol Dependencies

| Protocol                          | Component       | Usage                     |
|-----------------------------------|-----------------|---------------------------|
| `EFI_TCP4_SERVICE_BINDING`        | NetworkLib      | Create TCP child handles  |
| `EFI_TCP4_PROTOCOL`              | HttpClient/Server| TCP connections           |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`| FileTransferLib | Open volumes, file I/O    |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`| WebDavFsDxe     | Installed (produced)      |
| `EFI_FILE_PROTOCOL`              | FileTransferLib | Read, write, get info     |
| `EFI_FILE_PROTOCOL`              | WebDavFsDxe     | Implemented (produced)    |
| `EFI_IP4_CONFIG2_PROTOCOL`       | NetworkLib      | Check/configure IP        |
| `EFI_DHCP4_PROTOCOL`            | NetworkLib      | DHCP fallback             |
| `EFI_SIMPLE_NETWORK_PROTOCOL`   | NetworkLib      | NIC detection             |
| `EFI_BOOT_SERVICES->LoadImage`   | CmdMount.c     | Load WebDavFsDxe driver   |
| `EFI_BOOT_SERVICES->StartImage`  | CmdMount.c     | Start WebDavFsDxe driver  |

## Network Initialization

Pattern from SoftBMC (implemented in NetworkLib), with ConnectController
added for platforms that don't auto-connect the network stack:

1. Check for `EFI_IP4_CONFIG2_PROTOCOL` handles
2. If none found, enumerate `EFI_SIMPLE_NETWORK_PROTOCOL` (SNP) handles
   and call `ConnectController(SNP, NULL, NULL, TRUE)` to recursively
   load the network stack (MNP, ARP, IP4, TCP4, DHCP4) on top of SNP
3. If `-n <index>` specified, connect only that NIC (faster on multi-NIC)
4. Check link status via `SNP->Mode->MediaPresent` before DHCP — fail
   fast with a clear error if cable is unplugged
5. If `-i <ip>` specified, configure static IP on selected NIC
6. Otherwise, set DHCP policy via IP4Config2 and wait (up to 10s)
7. Fallback to `EFI_DHCP4_PROTOCOL` direct if IP4Config2 unavailable
8. Print IP address and return

If no NIC has an IP after configuration, print error and exit.

The `list-nics` command enumerates all SNP handles and prints NIC index,
MAC address, link status, and current IP (if configured). Use this to
identify which NIC index to pass with `-n`.

### Platform Compatibility: ConnectController vs Driver Loading

HttpFS uses a **ConnectController-only** approach — it assumes NIC
drivers are already present in firmware ROM and just needs to connect
the upper network stack layers. This is sufficient when:

- Firmware includes NIC ROM drivers (x86 workstations, most servers)
- PXE boot was attempted (pre-loads the full network stack)
- QEMU or other simulators (virtual NICs have built-in drivers)

**This approach will NOT work** on platforms where:

- Firmware has no NIC driver at all (no SNP handles to connect)
- Firmware's NIC driver locks the PCI handle but doesn't produce SNP
  (seen on some HP ZBook and Lenovo systems)

SoftBMC handles these cases with explicit driver loading: it calls
`LoadImage`/`StartImage` on external `.efi` NIC drivers from a
`\drivers\<arch>\` directory on the boot device, then does a targeted
`ConnectController` with only those loaded driver handles. It also
disconnects broken firmware drivers before rebinding (NIC takeover).

**For ARM64 server**: unknown whether firmware includes
working NIC drivers. To diagnose:

1. Boot HttpFS and run `list-nics`
2. If SNP handles appear with MAC addresses — ConnectController is
   sufficient, current approach works
3. If no handles appear — need to add SoftBMC-style driver loading
   (port `NetworkLoadDrivers()` from SoftBMC `Core/Network.c`)

## Implementation Plan

### Phase 1: Shared Libraries

Build the foundational libraries used by both `mount` and `serve`.

**NetworkLib:**
- NIC enumeration and selection (`-n` flag)
- DHCP configuration with IP4Config2 and DHCP4 fallback
- Static IP configuration (`-i` flag)
- `NetworkInit()`, `NetworkGetAddress()`, `NetworkCleanup()`

**HttpClientLib:**
- TCP4 connect to remote host:port
- HTTP/1.1 request building (GET, PUT, DELETE, POST)
- Response parsing (status, headers, body streaming)
- Range header support for resume
- Connection keep-alive and reuse
- `HttpClientConnect()`, `HttpClientRequest()`,
  `HttpClientReadBody()`, `HttpClientClose()`

**JsonLib:**
- Minimal JSON tokenizer (no DOM, no allocation)
- Extract string/number/boolean values by key
- Iterate arrays of objects
- `JsonParse()`, `JsonGetString()`, `JsonGetNumber()`,
  `JsonArrayNext()`

**Build files:**
- `.dec`, `.dsc`, `.inf` for package and all Phase 1 libraries
- `scripts/build.sh` for X64 and AARCH64

**xfer-server.py:**
- Python stdlib HTTP server (~200 lines)
- `GET /files/<path>` — download with Range support
- `PUT /files/<path>` — upload/create
- `DELETE /files/<path>` — delete
- `POST /files/<path>?mkdir` — create directory
- `GET /list/<path>` — JSON directory listing
- `GET /info` — server metadata
- `--root`, `--port`, `--bind`, `--read-only` flags

**Delivers:** Buildable libraries, workstation server script.

### Phase 2: Mount Command (WebDavFsDxe)

The killer feature — mount remote filesystem as UEFI volume.

**WebDavFsDxe driver:**
- Driver entry: parse URL from load options or protocol
- Call NetworkLib for TCP connection to workstation server
- Implement `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL->OpenVolume()`
- Implement full `EFI_FILE_PROTOCOL`:
  - `Open()` — HTTP GET /list/ to check existence
  - `Close()` — flush cache, no HTTP needed
  - `Read()` file — HTTP GET /files/ with Range for position
  - `Read()` directory — HTTP GET /list/, iterate entries
  - `Write()` — HTTP PUT /files/
  - `Delete()` — HTTP DELETE /files/
  - `GetInfo()` — file size/attributes from cached listing
  - `SetPosition()` / `GetPosition()` — local tracking
  - `Flush()` — no-op (writes are immediate)
- Directory cache (2s TTL, invalidate on write/delete)
- Read-ahead buffer (64 KB prefetch for sequential reads)
- Install protocol on new device handle with device path

**CmdMount.c (in HttpFS.efi):**
- `mount <url>` — locate WebDavFsDxe.efi (same directory as
  HttpFS.efi), call `LoadImage()` + `StartImage()` with URL
  in load options
- `umount <volume>` — locate driver handle by volume mapping,
  call `UnloadImage()`
- Print volume mapping after successful mount

**Delivers:** `HttpFS.efi mount http://10.0.0.5:8080/` creates FS1:
accessible from the UEFI Shell. Files on the workstation are instantly
visible and executable.

### Phase 3: Serve Command (HTTP)

HTTP file server on the UEFI host.

**HttpServerLib:**
- TCP4 listener on configurable port
- Connection pool (4 slots) with cooperative polling
- HTTP/1.1 request parsing (method, path, headers)
- Route table with method matching and permission flags
- Response API: `HttpSendResponse()`, `HttpSendHeaders()`,
  `HttpSendChunk()`
- Range header handling for resume
- `HttpServerInit()`, `HttpServerPoll()`, `HttpServerStop()`

**FileTransferLib:**
- Volume enumeration (all `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` handles)
- Directory listing as HTML and JSON
- Streaming file read in 8 KB chunks with progress callback
- Streaming file write in 8 KB chunks with progress callback
- SHA-256 computation during upload (inline, no second pass)
- File and empty directory deletion
- Directory creation (with parent creation)
- Recursive directory walk
- `FileTransferInit()`, `FileTransferListVolumes()`,
  `FileTransferReadFile()`, `FileTransferWriteFile()`, etc.

**CmdServe.c (in HttpFS.efi):**
- Wire route handlers: GET/PUT/DELETE/POST → FileTransferLib calls
- scp-style progress display with speed, percentage, bytes
- `--read-only` / `--write-only` enforcement
- `-t` idle timeout auto-exit
- Verbose request logging (`-v`)
- Main poll loop with ESC detection

**Client-side script:**
- `scripts/xfer.sh` — bash wrapper around curl for recursive
  upload/download of directory trees

**Delivers:** Full HTTP file server with progress display, resume,
and recursive transfer support.

### Phase 4: Serve WebDAV Extension

Add WebDAV protocol support to the serve command.

**HttpServerLib additions (WebDav.c):**
- PROPFIND handler — generate XML multistatus response with file
  properties (size, modified date, resource type)
- MKCOL handler — create directory (collection)
- MOVE handler — rename/move file or directory
- COPY handler — copy file or directory
- OPTIONS handler — return DAV compliance headers
- LOCK/UNLOCK stubs — always succeed (Windows compatibility)
- XML generation helpers for multistatus responses

**FileTransferLib additions (DirList.c):**
- WebDAV XML directory listing format
- File property gathering (creation time, content type)

**CmdServe.c additions:**
- `--webdav` flag to enable WebDAV routes
- Display mode in startup output

**Delivers:** Workstation can mount UEFI filesystem as network drive.

### Phase 5: Testing + Polish

- `scripts/qemu.sh` — QEMU launcher with port forwarding, boots
  HttpFS.efi and loads WebDavFsDxe.efi via startup.nsh
- `scripts/test.sh` — automated integration tests:

  **Mount tests:**
  1. Start xfer-server.py with test fixture directory
  2. Boot QEMU, run `HttpFS.efi mount http://host:port/`
  3. Verify `ls fs1:\` shows expected files
  4. Verify file read — copy from mounted volume, compare content
  5. Verify file write — create file, check on workstation
  6. Verify delete — remove file, check on workstation
  7. Verify executable — run `.efi` directly from mounted volume
  8. Verify umount — `HttpFS.efi umount fs1:`

  **Serve tests:**
  9. Boot QEMU with `HttpFS.efi serve`
  10. `GET /` — volume list contains `fs0`
  11. `PUT /fs0/test.txt` — upload, check 201 + X-SHA256
  12. `GET /fs0/test.txt` — download and compare
  13. `DELETE /fs0/test.txt` — verify 200, then 404
  14. `POST /fs0/testdir/?mkdir` — create directory
  15. Large file (1 MB) — upload/download, verify SHA-256
  16. Resume — partial download, then `Range: bytes=N-`
  17. Read-only mode — verify PUT returns 403

  **WebDAV tests:**
  18. PROPFIND — verify XML response structure
  19. MKCOL — create directory
  20. Mount from Linux (`mount -t davfs`)

- Test both X64 and AARCH64 in QEMU

**Delivers:** Automated test suite, both architectures verified.

## Design Decisions

### Mount First
The mount capability is the highest-value feature — it eliminates the
entire file transfer step from the UEFI development workflow. Build on
workstation, run immediately in UEFI Shell. Serve is still useful but
secondary.

### Simple HTTP/JSON Protocol for Mount (v1)
The mount driver uses plain HTTP with JSON directory listings instead
of full WebDAV. This avoids XML parsing on the UEFI side (~1500 fewer
lines). We control both sides (xfer-server.py + WebDavFsDxe), so
there's no interop concern. A future version can add WebDAV client
support for mounting standard WebDAV servers.

### Two Binaries (HttpFS.efi + WebDavFsDxe.efi)
`mount` needs a persistent protocol that survives after the command
returns. UEFI's mechanism for this is a DXE driver that stays resident.
The Shell app loads/unloads the driver, giving users a clean CLI
experience while being architecturally correct.

### No TLS
Diagnostic tool on isolated management networks. TLS adds mbedTLS
dependency (~200 KB), certificate management, and build time.
Not worth it for the use case.

### No Authentication
Same rationale. The tool runs interactively under the UEFI Shell — the
operator is physically or virtually present. If security is needed, run
on an isolated management VLAN.

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
Both mount (HttpClientLib) and serve (HttpServerLib) support the HTTP
`Range: bytes=N-` header. Critical for large transfers over flaky
management networks. `curl -C -` works out of the box with serve mode.

### Client-Side Recursive Transfer (Serve Mode)
The server handles one file per request. Recursive directory transfer is
handled by a client-side script (`xfer.sh`) that walks the JSON directory
listing and issues individual curl requests. This keeps the server simple.

### Connection Pool (Serve Mode)
UEFI has no threads. The server uses a fixed-size connection pool
(4 connections) with cooperative polling. The main loop polls all
connections and the keyboard (ESC). 4 connections is sufficient for
a single-user tool.

### SOC Library Split
NetworkLib, HttpClientLib, HttpServerLib, FileTransferLib, and JsonLib
are independent EDK2 libraries. Mount uses NetworkLib + HttpClientLib +
JsonLib. Serve uses NetworkLib + HttpServerLib + FileTransferLib. Both
benefit from the shared foundation without code duplication.

## SoftBMC Reference Patterns

HttpFS reuses proven patterns from the SoftBMC project (`../softbmc/`).
The table below maps each area to the SoftBMC source and notes what
changes for HttpFS.

| Area | Reuse From SoftBMC | Adapt for HttpFS |
|------|--------------------|--------------------|
| Build infra (.dec/.dsc) | `SoftBmcPkg.dec` structure, `SoftBmcPkg.dsc` LibraryClasses + `!include MdeLibs.dsc.inc` | Add `UefiDriverEntryPoint` for WebDavFsDxe. No CryptoPkg/MbedTls. |
| Build script | `scripts/build.sh` — arg parsing, EDK2 env setup, per-arch loop, binary summary | Remove embed-assets, driver copy, gen-compdb. |
| Network init | `Core/Network.c` — NIC enum via SNP, DHCP via IP4Config2 + DHCP4 fallback | Strip driver loading, DNS, API handler. Add ConnectController for ARM64. Add link check and `list-nics`. Single `NetworkInit()` call. |
| TCP | `Core/TcpClient.c` + `Core/TcpUtil.c` — connect pattern, 32 KB chunked send | Folded into HttpClientLib's TCP layer. |
| HTTP server | `Core/HttpServer.h` — connection pool, cooperative poll, route dispatch | Strip WebSocket, TLS, auth, cache. 4 connections vs 16. (Phase 3) |
| JSON | `Core/JsonParser.h` pattern (stack-allocated tokens, extract by key) | Custom tokenizer instead of JSMN. Add array iteration. |
| Coding style | Already matches — 4-space indent, K&R braces, PascalCase, mPrefix, `/** @file **/` | Copyright year 2026. |
| Logging | `SoftBmcLog()` ring buffer for long-running service | `Print()` directly — HttpFS is a CLI tool, not a service. |

## Estimated Size

| Component                | Lines (approx) |
|--------------------------|----------------|
| NetworkLib               | 250            |
| HttpClientLib            | 500            |
| HttpServerLib            | 700            |
| FileTransferLib          | 500            |
| JsonLib                  | 200            |
| WebDavFsDxe driver       | 800            |
| HttpFS application     | 400            |
| WebDAV extension         | 600            |
| xfer-server.py           | 200            |
| Build files (.dec/dsc/inf)| 200           |
| scripts/                 | 400            |
| **Total**                | **~4750**      |

## Future Enhancements (Not in v1)

### WebDAV Client for Mount
Upgrade the mount driver to speak standard WebDAV (PROPFIND/XML) in
addition to the simple JSON protocol. This would allow mounting any
standard WebDAV server (Apache mod_dav, IIS, etc.) without needing
xfer-server.py.

### Remote Command Execution
A companion DXE driver (`HttpFSCmdDxe`) watches a designated
directory (e.g., `fs0:\xfer\cmd\`) for `.nsh` files. When a script
appears (uploaded via WebDAV, HTTP PUT, or written through a mounted
volume), the driver executes it via `EFI_SHELL_PROTOCOL->Execute()`.
Output is captured to `<script>.out`. The remote user uploads a command,
then polls/downloads the output.

Security: command execution is opt-in (`--enable-cmd` flag).

### Other Items
- IPv6 support
- Drag-and-drop web UI (minimal JavaScript upload form)
- Transfer queue display (multiple concurrent transfers)
- Bandwidth throttling
