# HttpFS Design Document

## Overview

HttpFS is a UEFI toolkit for bidirectional file transfer and remote
filesystem access between a workstation and a UEFI host. Built with
the AXL SDK (no EDK2 build system). It provides two core capabilities:

1. **`mount`** — Mount a remote workstation directory as a UEFI volume
   (FSn:), enabling the UEFI Shell to directly access, execute, and
   modify files on the workstation in real time.

2. **`serve`** — Run an HTTP file server on the UEFI host, exposing
   local UEFI volumes to remote clients via `curl`, browser, or scripts.

**Goal:** Eliminate file transfer friction in UEFI development and
diagnostics. Build on your workstation, run immediately in the UEFI Shell.

## Use Cases

### Mount (workstation -> UEFI)

1. **Live development workflow** — mount a build output directory, run
   freshly compiled `.efi` files without any manual transfer step.
2. **ARM64 server bootstrapping** — mount tools directory on ARM64
   servers where BMC virtual media is unreliable.
3. **Bulk tool deployment** — mount a shared tools repository, run
   diagnostics directly from it.
4. **Live file editing** — edit files on the workstation, changes are
   instantly visible in the UEFI Shell.

### Serve (UEFI -> workstation)

5. **Upload EFI tools to a server** — push IpmiTool.efi, diagnostics,
   drivers, etc. via `curl` without virtual media or USB.
6. **Download logs/data from UEFI** — pull crash dumps, SMBIOS tables,
   or any file from the EFI System Partition.
7. **Remote directory browsing** — list filesystem contents from a browser.
8. **Recursive folder transfer** — upload or download entire directory
   trees for bulk deployment or retrieval.

## Architecture

Two binaries, two internal libraries, all using the AXL SDK:

```
+---------------------------------------------------------------+
|  Application / Driver Layer                                    |
|                                                                |
|  +------------------------+  +------------------------------+ |
|  | HttpFS.efi             |  | WebDavFsDxe.efi              | |
|  |                        |  |                              | |
|  | CLI: serve, mount,     |  | DXE driver (stays resident)  | |
|  |      umount, list-nics |  | EFI_FILE_PROTOCOL over HTTP  | |
|  | HTTP server (serve)    |  | Directory cache + read-ahead | |
|  | Driver loader (mount)  |  |                              | |
|  +------+---------+-------+  +------+--------+--------------+ |
|         |         |                 |        |                 |
|  Internal Libraries                                            |
|  +------v---+ +---v---------+ +----v---+ +--v--------------+ |
|  | network  | | transfer    | |  AXL SDK (external)         | |
|  |          | |             | |  axl_http_server_*           | |
|  | DHCP/IP  | | Volume enum | |  axl_http_client_*           | |
|  | via SDK  | | Stream R/W  | |  axl_json_*                  | |
|  |          | | Dir listing | |  axl_loop_*                  | |
|  +----+-----+ +------+------+ +----+--------+--------------+ |
|       |               |             |        |                 |
|  UEFI Firmware Protocols                                       |
|  +----v--------------+  +----------v--------v--------------+ |
|  | IP4Config2, DHCP4  |  | EFI_SIMPLE_FILE_SYSTEM            | |
|  | SNP (via SDK)      |  | EFI_FILE_PROTOCOL                 | |
|  +--------------------+  +-----------------------------------+ |
+---------------------------------------------------------------+
```

### Components

1. **HttpFS.efi** (Application) — `int main()` entry via AXL_APP.
   CLI parsing with `axl_args_parse`. Dispatches `serve`/`mount`/
   `umount`/`list-nics`. Serve uses AXL's `AxlHttpServer` with
   event loop. Mount uses `axl_driver_load`/`start`/`set_load_options`.

2. **WebDavFsDxe.efi** (DXE Driver) — `DriverEntry` with
   `axl_driver_init`. Implements `EFI_FILE_PROTOCOL` backed by
   AXL's HTTP client (`axl_http_request`). Uses `axl_json_parse`
   for directory listings, `axl_url_parse` for URL handling.

3. **network** (`src/net/`) — Thin wrapper around `axl_net_auto_init`
   for the common DHCP path. Retains static IP support via direct
   `IP4Config2` protocol calls.

4. **transfer** (`src/transfer/`) — Volume enumeration via
   `axl_volume_enumerate`, streaming file I/O via `axl_fopen`/
   `axl_fread`/`axl_fwrite`, directory listing as JSON/HTML.

### What comes from the AXL SDK

HTTP server, HTTP client, JSON parser/builder, event loop, hash tables,
argument parsing, URL parsing, IPv4 parsing, path resolution, driver
lifecycle, network auto-init, volume enumeration, wide-string helpers,
memory allocation, string utilities, logging.

## Mount Command

### How It Works

The `mount` command loads a companion DXE driver (`WebDavFsDxe.efi`)
that installs `EFI_FILE_PROTOCOL` on a new device handle. The UEFI
Shell sees this as a new volume (FSn:).

```
Workstation                              UEFI Host
+------------------+                    +----------------------+
| xfer-server.py   |  HTTP/JSON         | WebDavFsDxe.efi      |
|                  |<------------------>|                      |
| Serves files     |  GET/PUT/DELETE    | EFI_FILE_PROTOCOL    |
| from a local dir |  + JSON dir list   |                      |
|                  |                    | Appears as FS1:      |
| ~/share/         |                    |                      |
|  +-- tools/      |                    | Shell> ls fs1:\      |
|  |   +-- ipmi.efi|                    |   tools/             |
|  +-- builds/     |                    |   builds/            |
|      +-- diag.efi|                    |                      |
+------------------+                    +----------------------+
```

### Protocol (Simple HTTP/JSON)

The workstation runs `xfer-server.py` (provided with HttpFS). The
protocol is plain HTTP with JSON directory listings:

```
GET  /files/<path>        -> download file (with Range support)
PUT  /files/<path>        -> upload/create file
DELETE /files/<path>      -> delete file or empty directory
POST /files/<path>?mkdir  -> create directory (with parents)
POST /files/<path>?rename=<new> -> rename file
GET  /list/<path>         -> JSON directory listing:
                            [{"name":"foo.efi","size":48384,"dir":false,
                              "modified":"2026-03-17T14:30:00Z"}, ...]
GET  /info                -> server info: {"version":"1.0","root":"/share"}
```

### EFI_FILE_PROTOCOL Mapping

Every UEFI file operation translates to HTTP:

| EFI_FILE_PROTOCOL      | HTTP Request                        |
|-------------------------|-------------------------------------|
| `Open()`               | `GET /list/<parent>` (check exists) |
| `Close()`              | (no-op)                             |
| `Read()` (file)        | `GET /files/<path>` (Range header)  |
| `Read()` (dir)         | `GET /list/<path>` (iterate entries) |
| `Write()`              | `PUT /files/<path>`                 |
| `Delete()`             | `DELETE /files/<path>`              |
| `GetInfo(FILE_INFO)`   | `GET /list/<parent>` (find entry)   |
| `SetInfo(FILE_INFO)`   | `POST /files/<path>?rename=<new>`   |
| `GetPosition()`        | (local tracking, no HTTP)           |
| `SetPosition()`        | (local tracking, no HTTP)           |
| `Flush()`              | (no-op -- writes are immediate)     |

### Caching Strategy

1. **Directory cache** — cache `GET /list/<path>` results for 2 seconds.
   Invalidated on any write/delete to that directory. Entries are copied
   into each file handle to avoid stale pointer issues during cache
   eviction. 16 slots, LRU eviction.

2. **Read-ahead buffer** — when reading a file, prefetch 64 KB into a
   buffer. Sequential reads hit the buffer instead of issuing per-8KB
   HTTP requests.

No write caching — writes go through immediately.

### CLI

```
HttpFS mount <url> [-r]
HttpFS umount

Examples:
  HttpFS mount http://10.0.0.5:8080/
  HttpFS umount
```

### xfer-server.py (Workstation Side)

A self-contained Python script that serves a local directory over HTTP
with the JSON listing API. No dependencies beyond Python stdlib:

```bash
./scripts/xfer-server.py
./scripts/xfer-server.py --root /path/to/share --port 9090 --bind 0.0.0.0
./scripts/xfer-server.py --read-only
```

## Serve Command

### REST API

All paths use the format `/<volume>/<path>`. Volume names match UEFI
Shell conventions: `fs0`, `fs1`, etc.

| Method   | Path                  | Description                      |
|----------|-----------------------|----------------------------------|
| `GET /`  |                       | List all volumes (JSON/HTML)     |
| `GET`    | `/<vol>/<path>`       | Download file or list directory  |
| `PUT`    | `/<vol>/<path>`       | Upload (create/overwrite) file   |
| `DELETE` | `/<vol>/<path>`       | Delete file or empty directory   |
| `POST`   | `/<vol>/<path>?mkdir` | Create directory                 |

Content negotiation: `Accept: application/json` returns JSON, otherwise
HTML with dark-theme directory browser.

### Headers

| Header              | Direction | Description                         |
|---------------------|-----------|-------------------------------------|
| `Content-Length`    | Both      | File size                           |
| `Range`            | Request   | Byte range for partial download     |
| `Content-Range`    | Response  | Byte range in 206 response          |
| `Accept`           | Request   | `application/json` for JSON listing |

### CLI

```
HttpFS serve [options]

Options:
  -p <port>        Listen port (default: 8080)
  -n <index>       NIC index
  -t <seconds>     Idle timeout
  --read-only      Block uploads/deletes
  --write-only     Block downloads
  -v               Verbose logging
  -h               Show help

Press ESC to stop the server.
```

### Examples

```bash
curl http://192.168.1.100:8080/                              # list volumes
curl http://192.168.1.100:8080/fs0/EFI/Boot/bootaa64.efi -o boot.efi
curl -T IpmiTool.efi http://192.168.1.100:8080/fs0/IpmiTool.efi
curl -X DELETE http://192.168.1.100:8080/fs0/temp/old.log
curl -X POST "http://192.168.1.100:8080/fs0/tools/?mkdir"
curl -C - http://192.168.1.100:8080/fs0/large.bin -o large.bin  # resume
```

## Project Layout

```
src/
  app/                         CLI application
    main.c                     Entry point, subcommand dispatch
    cmd-serve.c                HTTP file server (AxlHttpServer + AxlLoop)
    cmd-mount.c                Mount/umount (axl_driver_load/start/unload)
    httpfs-internal.h          Shared command declarations
  driver/                      DXE driver
    webdavfs.c                 Driver entry, URL parsing, protocol install
    webdavfs-file.c            EFI_FILE_PROTOCOL (11 functions)
    webdavfs-cache.c           Directory cache + HTTP request helper
    webdavfs-internal.h        Private types
  net/                         Network initialization
    network.c                  Wrapper around axl_net_auto_init
    network.h                  Public API
  transfer/                    Local file operations
    file-transfer.c            Volume enum, streaming read/write
    file-transfer.h            Public API
    dir-list.c                 Directory listing as JSON/HTML
scripts/
  test.sh                      Integration tests (host + QEMU)
  xfer-server.py               Workstation file server for mount
docs/
  Design.md                    This document
Makefile                       axl-cc build
CLAUDE.md                      Project instructions for Claude Code
```

## Network Initialization

Uses `axl_net_auto_init()` from the AXL SDK, which handles:
1. Loading NIC drivers from `\drivers\{arch}\` if needed
2. Connecting SNP handles to bring up IP4/TCP4 stack
3. DHCP via IP4Config2 with polling
4. DHCP4 direct fallback

Static IP is handled locally via `IP4Config2->SetData()` after
the SDK brings up the network stack.

The `list-nics` command uses `axl_net_list_interfaces()` directly.

## Design Decisions

### Mount First
The mount capability is the highest-value feature. Build on workstation,
run immediately in UEFI Shell.

### Simple HTTP/JSON Protocol
The mount driver uses plain HTTP with JSON directory listings instead
of full WebDAV XML. Avoids XML parsing on the UEFI side. We control
both sides (xfer-server.py + WebDavFsDxe).

### Two Binaries (HttpFS.efi + WebDavFsDxe.efi)
`mount` needs a persistent protocol that survives after the command
returns. UEFI's mechanism is a DXE driver that stays resident. The
app loads/unloads the driver.

### AXL SDK
All HTTP, JSON, event loop, hash table, TCP, and network functionality
comes from the AXL SDK. HttpFS has no local reimplementations of these.
The project is ~3,000 lines (app + driver + libraries), down from
~4,800 lines in the EDK2 version.

### No TLS / No Authentication
Diagnostic tool on isolated management networks. TLS and auth add
complexity without value for the use case.

### Streaming I/O
Files are transferred in 8 KB chunks, never fully buffered in RAM
(except for Range responses where the full file is read for slicing).
Progress tracking hooks into streaming callbacks.

## Build and Test

```bash
make                           # X64
make ARCH=aa64                 # AARCH64
scripts/test.sh                # Host-side tests (24)
scripts/test.sh --qemu         # Full suite with QEMU (47 tests)
```

## Future Enhancements

- WebDAV serve mode (PROPFIND, MKCOL, MOVE, COPY for network drive mount)
- WebDAV client for mount (mount standard WebDAV servers without xfer-server.py)
- TLS support (SDK has `axl_http_client_set("tls.verify", ...)`)
- AARCH64 QEMU test automation (`--aarch64` flag)
- IPv6 support
