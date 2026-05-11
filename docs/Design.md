# axl-webfs Design Document

## Overview

axl-webfs is a UEFI toolkit for bidirectional file transfer and remote
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

One distributable launcher binary that bundles two AxlService
driver images (both embedded via `axl-cc --embed`), plus two
internal libraries. All UEFI plumbing comes from the AXL SDK.
The drivers are also emitted as standalone `.efi` files for users
who prefer the UEFI-shell `load` workflow.

Each service follows the single-source-file dual-compile pattern
from axl-sdk's `service-demo-custom.c`: one `.c` per service,
compiled twice (once with `-DAXL_SERVICE_BUILD_DRIVER` for the
driver image's setup/teardown body + `AXL_SERVICE_DRIVER` entry,
once without for the launcher's descriptor stub used by
`axl_service_start_embedded` to serialize LoadOptions).

```
+--------------------------------------------------------------------+
|  Application / Driver Layer                                         |
|                                                                     |
|  +-------------------------+    +----------------------------+ +----------------------------+
|  | axl-webfs.efi           |    | axl-webfs-serve-dxe.efi    | | axl-webfs-mount-dxe.efi    |
|  |                         |    | (AxlService driver)        | | (AxlService driver)        |
|  | CLI dispatch:           |    | setup: HTTP server + routes| | setup: install             |
|  |   serve / serve-stop    |--->| teardown: free everything  | |   EFI_FILE_PROTOCOL        |
|  |   mount / umount        |    | tick: 50ms (drives loop)   | | teardown: uninstall + free |
|  |   list-nics             |--->|                            | | tick: 1s (no sources)      |
|  +-------------------------+    +----------------------------+ +----------------------------+
|        |              |                  |                                 |
|  Shared internal libraries                                                  |
|  +--------+  +----------+  +--------------------------------------+        |
|  | network|  | transfer |  | AXL SDK (external)                   |        |
|  | DHCP/IP|  | Volume + |  | axl_http_server / _client / _json    |        |
|  | wrapper|  | Stream IO|  | axl_loop / axl_service / axl_url     |        |
|  +--------+  +----------+  +--------------------------------------+        |
+--------------------------------------------------------------------+
```

### Components

1. **axl-webfs.efi** (Launcher) — `int main()` entry via AXL_APP.
   CLI parsing with `axl_args_parse`. Dispatches `serve`/`serve-stop`/
   `mount`/`umount`/`list-nics` to verb handlers. Each handler
   populates the matching opts struct from AxlArgs and calls
   `axl_service_start_embedded` (or `axl_service_stop`) against the
   matching deploy descriptor. The launcher carries no service body
   itself -- both driver images run from their own `.efi` blob
   inside the launcher binary.

2. **axl-webfs-serve-dxe.efi** (AxlService DXE driver) — built from
   `src/serve/webfs-serve.c` with `-DAXL_SERVICE_BUILD_DRIVER`.
   `setup()` brings up the network, starts an `AxlHttpServer` on
   the configured port, registers six route handlers, and
   subscribes to a pubsub topic for non-GET request console
   feedback. `teardown()` reverses it. Tick fires every 50 ms to
   drive the loop.

3. **axl-webfs-mount-dxe.efi** (AxlService DXE driver) — built from
   `src/mount/webfs-mount.c` with `-DAXL_SERVICE_BUILD_DRIVER`.
   `setup()` parses the URL, brings up the network, validates the
   server with `GET /info`, then installs `EFI_FILE_PROTOCOL` on a
   new handle (vendor device path so `umount` can find it).
   `teardown()` uninstalls and frees. Tick fires every 1000 ms;
   `EFI_FILE_PROTOCOL` callbacks run synchronously from the Shell's
   caller, not on the loop.

4. **network** (`src/net/`) — Thin wrapper around `axl_net_auto_init`
   for the common DHCP path. Retains static IP support via direct
   `IP4Config2` protocol calls. Linked into the launcher (for
   `list-nics`) and both driver images.

5. **transfer** (`src/transfer/`) — Volume enumeration via
   `axl_volume_enumerate`, streaming file I/O via `axl_fopen`/
   `axl_fread`/`axl_fwrite`, directory listing as JSON/HTML.
   Driver-only (linked into the serve driver image only).

### What comes from the AXL SDK

HTTP server, HTTP client, JSON parser/builder, event loop, hash tables,
argument parsing, URL parsing, IPv4 parsing, path resolution, driver
lifecycle, network auto-init, volume enumeration, wide-string helpers,
memory allocation, string utilities, logging.

## Mount Command

### How It Works

The `mount` command starts the embedded `axl-webfs-mount-dxe.efi`
AxlService via `axl_service_start_embedded`. The driver's
`mount_setup` parses the URL passed via LoadOptions, validates the
server, and installs `EFI_FILE_PROTOCOL` on a new device handle so
the UEFI Shell sees a new volume (FSn:). `umount` calls
`axl_service_stop`, which resolves the running driver image by the
service's name-derived GUID and unloads it; the driver's unload
stub runs `mount_teardown` to uninstall the protocols.

```
Workstation                              UEFI Host
+------------------+                    +----------------------+
| xfer-server.py   |  HTTP/JSON         | axl-webfs-mount-dxe.efi    |
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

The workstation runs `xfer-server.py` (provided with axl-webfs). The
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
axl-webfs mount <url> [-r]
axl-webfs umount

Examples:
  axl-webfs mount http://10.0.0.5:8080/
  axl-webfs umount
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
axl-webfs serve [options]
axl-webfs serve-stop

Options:
  -p <port>        Listen port (default: 8080)
  -n <index>       NIC index
  -t <seconds>     Idle timeout
  --mode <m>       Permission mode: read-write (default),
                   read-only (block PUT/POST/DELETE),
                   write-only (block GET)
  --source <ip>    Bind listener to interface with this station IPv4
  -l, --log <path> Tee axl_log output to a file (e.g. fs0:\webfs.log).
                   Open failure surfaces a console error and serve
                   continues with console-only output -- a missing
                   log destination doesn't bring down the server.
                   Uses axl_log_file_attach so output is structured
                   ([LEVEL] [domain] message) and includes the
                   request log lines from the pubsub feedback path.
  -v               Verbose logging
  -h               Show help

`serve` always runs as a resident DXE driver and returns to the
shell. Use `serve-stop` to stop it. (`unload -n
axl-webfs-serve-dxe.efi` from the Shell also works.)
```

### How serve works

`axl-webfs.efi serve` populates `g_serve_opts` from the parsed
flags and hands an `AxlServiceDeploy` to
`axl_service_start_embedded`, which LoadImages the embedded
`axl-webfs-serve-dxe.efi` blob (spliced into `.rodata` by
`axl-cc --embed`) with the options serialized into LoadOptions.
The driver's `AXL_SERVICE_DRIVER` macro decodes LoadOptions back
into its own `g_serve_opts`, runs `serve_setup` against a
driver-mode loop, and the launcher returns to the shell. The
HTTP server runs inside the driver image until `serve-stop`
unloads it.

`src/serve/webfs-serve.c` is dual-compiled. The driver build
(`-DAXL_SERVICE_BUILD_DRIVER`) gets the full body (route handlers,
permission middleware, `serve_setup`, `serve_teardown`,
`AXL_SERVICE_DRIVER` entry); the launcher build gets just the
descriptor stub (`g_serve_opts`, `serve_descs`, `webfs_serve` with
NULL setup/teardown) that `axl_service_start_embedded` reads to
serialize LoadOptions.

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
  app/                         Launcher (axl-webfs.efi)
    main.c                     Entry point, AxlArgs verb dispatch
    cmd-serve.c                serve / serve-stop verbs (axl_service_*)
    cmd-mount.c                mount / umount verbs (axl_service_*)
  serve/                       Serve service (single-file dual-compile)
    webfs-serve.c              AxlService descriptor + setup/teardown +
                               route handlers, gated on
                               AXL_SERVICE_BUILD_DRIVER
    webfs-serve.h              Public extern decls (ServeOpts, webfs_serve)
    upload-asset.c/.h          Embedded upload UI assets (driver-only)
    webfs-dav.c/.h             WebDAV class-1 + MOVE adapter onto ft_*
                               (driver-only; mounts /dav)
  mount/                       Mount service (single-file dual-compile)
    webfs-mount.c              AxlService descriptor + setup/teardown,
                               gated on AXL_SERVICE_BUILD_DRIVER
    webfs-mount.h              Public extern decls (MountOpts, webfs_mount)
    webfs-file.c               EFI_FILE_PROTOCOL impl (driver-only)
    webfs-cache.c              Directory cache + HTTP request helper
                               (driver-only)
    webfs-internal.h           Driver-only types (WebFsPrivate, etc.)
  net/                         Network initialization (shared)
    network.c                  Wrapper around axl_net_auto_init
    network.h                  Public API
  transfer/                    Local file operations (used by serve)
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

Build outputs (per arch, in `out/<arch>/`):

- `axl-webfs.efi` — application; embeds both drivers via `axl-cc --embed`
- `axl-webfs-mount-dxe.efi` — mount driver (built then embedded into the app;
  also emitted as a standalone .efi for `load`-from-shell workflows)
- `axl-webfs-serve-dxe.efi` — serve driver (same: built, embedded, and
  also emitted standalone)

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
both sides (xfer-server.py + axl-webfs-mount-dxe).

### WebDAV on the Serve Side
Serve mode exposes the volume tree twice: as the curl-friendly
REST surface under `/<vol>/<path>` (HTML/JSON listings,
`POST ?mkdir`, etc.) AND as RFC 4918 class-1 WebDAV (plus MOVE)
under `/dav` for Finder, Explorer, davfs2, and cadaver clients.
The WebDAV layer lives entirely in axl-sdk
(`axl_http_server_add_webdav`); axl-webfs supplies an
`AxlWebDavOps` adapter (`src/serve/webfs-dav.c`) that maps the
12 callbacks onto the existing `ft_*` helpers. The mount root
enumerates volumes as virtual collections; everything under
`/dav/<vol>/` maps onto the matching `FtVolume`. MOVE goes
through `axl_file_move`: atomic rename on the same-directory
path and chunked stream copy + source delete on the cross-
directory path. PROPFIND populates `<D:getlastmodified>` from
`AxlDirEntry.mtime_unix` so Finder caches stay coherent.

### Two AxlServices, One Launcher Binary
`mount` and `serve` both need a persistent protocol that survives
after the command returns, so axl-webfs ships two DXE drivers. Each
driver is an AxlService (single-source-file dual-compile pattern,
mirroring axl-sdk's `service-demo-custom.c`): one `.c` per service,
compiled twice from the same source -- once with
`-DAXL_SERVICE_BUILD_DRIVER` for the driver image (full body +
`AXL_SERVICE_DRIVER` entry), once without for the launcher
(descriptor stub only, used by `axl_service_start_embedded` to
serialize LoadOptions). Both driver images are `.incbin`'d into
`axl-webfs.efi` via `axl-cc --embed` so the toolkit ships as a
single distributable file. The launcher's verb handlers
(serve / serve-stop / mount / umount) all reduce to the same
shape: populate the matching opts struct from AxlArgs, then call
`axl_service_start_embedded` (or `axl_service_stop`). The two
services use distinct descriptor names, so each gets its own
deterministic `axl_guid_v5` identity GUID for `is_running` /
`stop` lookups. Standalone `-dxe.efi` files for both drivers are
still emitted by the build for users who prefer the UEFI-shell
`load` workflow.

### AXL SDK
All HTTP, JSON, event loop, hash table, TCP, and network functionality
comes from the AXL SDK. axl-webfs has no local reimplementations of these.
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
scripts/test.sh                # Host-side tests (xfer-server.py)
scripts/test.sh --qemu         # Full suite with QEMU integration
scripts/test.sh --aarch64      # Same, also runs the AARCH64 mount test
```

## Future Enhancements

*(Multi-GB ISO transfers are now supported in both directions:
PUT via `axl_http_server_add_upload_route` (per-chunk bytes
land in the upload handler as they arrive on the wire, no full-
body buffering, body_limit bypassed), and GET via
`axl_http_response_set_streamer` (the dispatcher pulls chunks
from `get_streamer_pull` and finalizes via `get_streamer_close`
on EOF, error, or connection reset, so the FtReadCtx never
leaks). Range requests open the file at `range.start` and bound
the streamer to the slice length.)*
- WebDAV client for mount (mount standard WebDAV servers without xfer-server.py)
- TLS support (SDK has `axl_http_client_set("tls.verify", ...)`)
- IPv6 support
