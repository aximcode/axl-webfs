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

1. **axl-webfs.efi** (Launcher) — plain `int main(int argc, char **argv)`
   handing off to `axl_args_run`. Dispatches `serve`/`serve-stop`/
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
   `setup()` parses the URL, brings up the network via
   `axl_net_init_from_opts`, validates the server, then publishes
   an `AxlFsProvider` (callback table speaking UTF-8 paths +
   `AxlFsStatus`) via `axl_fs_provider_publish`. The SDK
   synthesizes `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` +
   `EFI_FILE_PROTOCOL` on top of that callback table and installs
   them on a fresh vendor-path handle. `teardown()` reverses it.
   Tick fires every 1000 ms; `AxlFsProvider` callbacks run
   synchronously from the Shell's caller, not on the loop.

4. **transfer** (`src/transfer/`) — Volume enumeration via
   `axl_volume_enumerate`, streaming file I/O via `axl_fopen`/
   `axl_fread`/`axl_fwrite`, directory listing as JSON/HTML.
   Driver-only (linked into the serve driver image only). Networking
   is the SDK's responsibility — no per-project wrapper since the
   AxlNetOpts uptake.

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
server, and calls `axl_fs_provider_publish` with a callback table
that speaks UTF-8 paths + `AxlFsStatus` return codes. The SDK
synthesizes `EFI_FILE_PROTOCOL` + `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`
on top, installs them on a fresh vendor-path handle, and marshals
CHAR16 ↔ UTF-8 + `EFI_FILE_INFO` headers/trailers + `EFI_STATUS`
mapping at the boundary — so the UEFI Shell sees a new volume
(FSn:). `umount` calls `axl_service_stop`, which resolves the
running driver image by the service's name-derived GUID and
unloads it; the driver's unload stub runs `mount_teardown` to
unpublish the provider.

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

### AxlFsProvider Mapping

The mount driver supplies an `AxlFsProvider` callback table; the
SDK invokes the callbacks for each `EFI_FILE_PROTOCOL` op the
Shell makes, marshalling CHAR16 paths to UTF-8 and the result
back. The callbacks issue HTTP requests via `axl_http_client`
(JSON protocol shown below; the parallel WebDAV path uses the
RFC 4918 verbs instead — both go through the same vtable):

| AxlFsProvider callback | HTTP Request                       |
|------------------------|------------------------------------|
| `open` (existing)      | `GET /list/<parent>` (check exists) |
| `open` (create)        | `PUT /files/<path>` (empty body) or `POST ?mkdir` |
| `close`                | flushes any buffered PUT body      |
| `read` (file)          | `GET /files/<path>` (Range header) |
| `read` (dir)           | `GET /list/<path>` (iterate entries) |
| `write`                | accumulates; final `PUT /files/<path>` (streamed) |
| `remove`               | `DELETE /files/<path>`             |
| `stat`                 | `GET /list/<parent>` (find entry)  |
| `rename`               | `POST /files/<path>?rename=<new>`  |
| `SetPosition()`        | (local tracking, no HTTP)           |
| `Flush()`              | (no-op -- writes are immediate)     |

**Name length is a skip rule.** The mount holds an entry name in 256
bytes (`WEBFS_NAME_SIZE`, `src/mount/webfs-internal.h`): 255 bytes of
UTF-8 and the NUL, matching the SDK's `AxlFsEntry.name`. A remote name
longer than that -- possible from any server whose filesystem allows
255 UTF-16 units -- is left out of the listing with a warning, and an
`open`/create or rename by such a name fails. It is never cut: a cut
name can equal a sibling's, which then appears twice and opens in the
other's place. A WebDAV `<href>` is decoded whole, however long its
percent-encoding, and only the decoded path is held to the limit. The
buffers are to be sized from the SDK's `AXL_FS_NAME_MAX` once it exists.

**Path length is refused, never cut.** A mount path is at most 511
bytes (`MAX_PATH_LEN`); `open` and `rename` refuse a longer one with an
error. Each request is checked once more as it is built: the SDK's HTTP
client writes the request target (encoded path plus query) into 512
bytes and silently cuts a longer one, so `webfs_request_url` refuses a
target that would not fit. A cut path names another entry -- in the
JSON protocol `/files` plus a 511-byte path cut to 511 IS `/files` plus
its 505-byte prefix -- whose bytes would be read, or overwritten,
instead. In practice the request target is the tighter limit: a JSON
mount reaches paths up to 505 bytes (less once percent-encoded).

Known upstream cut, not solved here: the SDK's fs-provider converts an
EFI file name into 512 bytes and CUTS a longer one (`resolve_efi_path`)
before `webfs_open` sees it, so the mount's own `MAX_PATH_LEN` refusal
cannot fire through it, and over WebDAV -- no `/files` prefix to push
the request past the limit -- a cut 511-byte path would be sent. The fix
belongs in axl-sdk (refuse the name, never cut it). The UEFI Shell does
not reach it today: it fails a 520-byte `type` before any open reaches
the mount, which the `long path (dav)` test pins; that test reports the
cut as an expected failure, naming the SDK fix, should it ever be
reached.

### Caching Strategy

1. **Directory cache** — cache `GET /list/<path>` results for 2 seconds.
   Invalidated on any write/delete to that directory. Entries are copied
   into each file handle to avoid stale pointer issues during cache
   eviction. 16 slots, LRU eviction. A listing is cached WHOLE, however
   big: the protocols append every entry to an `AxlArray`, the JSON body
   is parsed at its full size, and the cache (`webfs-cache.c`) owns each
   listing's array -- not the SDK's `AxlCache`, whose fixed-size values
   could not hold one, and which has no hook to free one it evicts.
   Whole is not unbounded: a listing past `WEBFS_LIST_MAX_ENTRIES`
   (16384, ~5 MB of entries; counted in entries because a byte cap would
   admit very different directories as JSON and as PROPFIND XML) is
   refused with an error to the Shell and a log line, never cut, and
   every expired slot is swept on each lookup, so an unvisited big
   listing does not stay resident. The HTTP body itself is not capped:
   the SDK's client has no response-size limit to set.

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
  --listen-ip <ip> Bind listener to interface with this station IPv4
  -a, --auth <u:p> Require HTTP Basic auth (user:pass) on every surface
                   -- REST routes, streaming uploads, and the /dav
                   mount. Wired through the SDK's add_route_auth /
                   add_upload_route_auth / add_webdav_auth and a
                   credential-checking AxlAuthCallback.
                   axl_http_server_set_auth_challenge makes each 401
                   carry WWW-Authenticate: Basic, so browsers, Finder,
                   and Explorer prompt for credentials. Pair with --tls
                   so the credential isn't sent in cleartext.
  -s, --tls        Serve over HTTPS via axl_http_server_use_tls. A
                   self-signed ECDSA P-256 cert is generated on boot
                   (axl_tls_generate_self_signed, bound IPv4 in the
                   SAN) unless --cert/--key supply a DER pair. Needs an
                   AXL_TLS=1 SDK build (the default for this project;
                   the ~200 KB lands in the serve driver only).
  --cert <path>    DER certificate path (e.g. fs0:\cert.der). Must be
  --key <path>     given together with --key; omit both for self-signed.
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

# HTTPS + auth; verify upload integrity end to end:
curl -k -u admin:s3cret https://192.168.1.100:8080/fs0/         # over TLS
H=$(sha256sum tool.efi | awk '{print $1}')
curl -k -u admin:s3cret -H "Content-Digest: sha-256=$H" \
     -T tool.efi https://192.168.1.100:8080/fs0/tool.efi        # 400 on mismatch
```

Upload integrity: when a `PUT` carries a `Content-Digest` (RFC 9530,
`sha-256` or `sha-512`, `:<base64>:` or bare hex; the strongest listed
is checked), `handle_put_chunk` hashes the streamed body through an
`AxlChecksum` and compares it to the advertised value BEFORE the commit
— a mismatch, or a value that cannot be read, discards the write and
returns `400`; a hash that cannot be allocated is `500`, never an
unchecked commit. The header is read exactly as the SDK's /dav PUT
reads it, by a copy of its parser (`content_digest_claim`): the SDK's is
static, and should be exported. The undocumented `sha256=` spelling the
old REST parser also took is gone with it. This is the upload-side peer of
the `Want-Digest`/`Digest` emission already done on `GET`. The WebDAV
`/dav` surface answers the same header with the same `400`; there the
SDK checks it itself, also before the commit. Both PUTs are atomic
(below), so a rejected body leaves the previous file exactly as it was.

## Project Layout

```
src/
  webfs-version.h              Version string
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
    webfs-dav.c/.h             WebDAV class-1 + MOVE: virtual volume root
                               over axl_fs_webdav_ops (driver-only;
                               mounts /dav)
  mount/                       Mount service (single-file dual-compile)
    webfs-mount.c              AxlService descriptor + setup/teardown,
                               gated on AXL_SERVICE_BUILD_DRIVER
    webfs-mount.h              Public extern decls (MountOpts, webfs_mount)
    webfs-file.c               AxlFsProvider callback table impl
                               (open/close/read/write/...) — driver-only
    webfs-cache.c              Directory cache + HTTP request helper +
                               digest header parser (driver-only)
    webfs-protocol.h           Protocol vtable: JSON vs WebDAV
    webfs-protocol-json.c      Bespoke HTTP/JSON protocol impl
    webfs-protocol-dav.c       RFC 4918 WebDAV client impl
    webfs-internal.h           Driver-only types (WebFsPrivate, etc.)
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

Build outputs (per arch, in `build/axl/<arch>/`):

- `axl-webfs.efi` — application; embeds both drivers via `axl-cc --embed`
- `axl-webfs-mount-dxe.efi` — mount driver (built then embedded into the app;
  also emitted as a standalone .efi for `load`-from-shell workflows)
- `axl-webfs-serve-dxe.efi` — serve driver (same: built, embedded, and
  also emitted standalone)

## Network Initialization

Uses `axl_net_init_from_opts()` from the AXL SDK, driven by an
`AxlNetOpts` that the CLI populates (`--nic`, `--listen-ip` /
`--source-ip`) and that AxlService serializes through to the driver via
LoadOptions. The SDK handles:
1. Loading NIC drivers from `\drivers\{arch}\` if needed
2. Connecting SNP handles to bring up the IP4/TCP4 stack
3. DHCP via IP4Config2 with polling
4. DHCP4 direct fallback

Static IP is part of that call — `AxlNetOpts.local_ip` — not a local
`IP4Config2->SetData()` step. There is no per-project network library.

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
`AxlWebDavOps` adapter (`src/serve/webfs-dav.c`). The mount root
enumerates volumes as virtual collections; everything under
`/dav/<vol>/` that touches one path -- PROPFIND stat, GET/HEAD, PUT,
MKCOL, DELETE -- is forwarded to the SDK's own filesystem backend,
`axl_fs_webdav_ops()`, over one `AxlFsRoot` (`"<vol>:"`) per volume.
Directory listing stays in `webfs-dav.c`: an `AxlFsRoot` at a bare
volume strips the trailing separator and its `list_dir` opens `"fs0:"`,
which lists nothing. PROPFIND populates `<D:getlastmodified>`
from `AxlFsEntry.mtime_unix` so Finder caches stay coherent.

A `/dav` PUT is atomic, by that forwarding. The body is written to a
temp sibling (`~APxxxxx.TMP`) and replaces the target with
`axl_file_replace` only when the SDK commits it
(`write_close(false)`); a discard (`write_close(true)`: the client hung
up, a chunk failed, the `Content-Digest` did not match) deletes the
temp through the writer's own handle and leaves the old file exactly as
it was. While a PUT is in flight its temp is no resource, on either
surface: a /dav request naming it finds nothing (the SDK), a REST GET,
HEAD, DELETE or `?mkdir` naming it is 404 (`webfs_live_temp`), a REST PUT onto it is 403 (the SDK's write path
refuses it), a /dav MOVE onto it is 403, and the
PROPFIND and REST listings (`ft_set_list_filter`) leave it out -- a
temp-shaped name the SDK's stat refuses is a live temp, since the SDK
keeps its registry private. EVERY component of a path is checked, split
on each separator the volume honours ('/', '\\', ':'), so a trailing
slash or another letter case still names the temp; and a REST path that
decodes to a '\\' or ':' (`%5C`, `%3A`) is 400, as is a /dav path
holding one -- they are UEFI path syntax, not name bytes. So is a path with a component that starts with a blank or ends with a
blank or period (`<temp>.`, `<temp>%20`, `%20<temp>`, and `.`/`..`):
the FAT driver trims those before its lookup, so they would open the
temp under a spelling no exact-name check matches, and FAT can store no
such name. (The SDK's own live-temp check has the same gap.) `serve -v` logs the temp each PUT streams
to ("PUT fs0:/a.txt: body streams to temp ~AP1A2B3.TMP"), which names
the file a power loss could leave behind. This is not rebuilt in
axl-webfs on purpose: deleting through the writer's handle and flushing
with it still open are SDK-internal, so a consumer re-implementation
over the public `AxlFileWriter` could only delete by path. A commit
drops the path's cached digest, which is keyed by size and would
otherwise survive a same-size replace.

MOVE stays in axl-webfs because it crosses volumes, which no single
`AxlFsRoot` can: it goes through `axl_file_move` (a rename within a
volume, chunked copy + source delete across), and its status is
passed back for the SDK to map. The SDK answers a missing source and
an existing destination under `Overwrite: F` before the op runs. The
mount does not set `AXL_WEBDAV_CAP_IDENTITY`: MOVE does not decide
identity by what the volume stores, so a case-only rename with
`Overwrite: F` is refused 412 rather than forwarded. Want-Digest stays
here too, to share `webfs-serve.c`'s digest cache with the REST
surface.

The REST `PUT /<vol>/<path>` is atomic the same way and by the same
code: `handle_put_chunk` drives `webfs_put_open` / `webfs_put_write` /
`webfs_put_close` (`webfs-dav.h`), the wrapper over that SDK write path
-- one atomic writer, not two. It keeps the REST PUT's one extra: a
missing parent directory (one level) is created for the upload -- and
taken back, while still empty, if that PUT then fails for any reason
(refused, mismatched, hung up), so a failed PUT leaves the tree as it
was; a parent that was already there stays. Like
/dav, the REST surface takes one PUT at a time: its upload state is
keyed to the request's `request_id`, a PUT that overlaps it (another
client, uploads interleave across connections) is answered 503 with
`Retry-After: 1` and touches nothing, and another connection's hang-up
cannot discard the PUT in flight.

Request paths on the serve side are refused, never cut, like the
mount's: `parse_volume_path` checks the percent-decode, answers 414 for
a path (or its `<vol>:<sub>` UEFI form, `ft_build_path`) that does not
fit and 400 for one that decodes to an embedded NUL; `parse_dav_path`
refuses the same for /dav. The ft_* helpers build their UEFI paths
through the same checked `ft_build_path`.

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
The project is ~5,300 lines of C across the launcher, both service
drivers, and the transfer layer.

### TLS and Authentication: Opt-In, Off by Default
This started as a diagnostic tool for isolated management networks, where
TLS and auth were judged pure complexity. Both have since shipped, because
"isolated management network" turned out to be an assumption about the
deployment rather than a property of it:

- `serve --tls` (optionally `--cert`/`--key`; self-signed when omitted).
  The ~200 KB of TLS lands in the serve driver only, and the SDK is built
  with `AXL_TLS=1` by default — `make AXL_TLS=` drops it, after which
  `--tls` fails at runtime rather than silently serving plaintext.
- `serve --auth user:pass` (HTTP Basic), which also emits
  `WWW-Authenticate` on 401 so browsers prompt. A colon-less value is
  rejected by the verb handler before the service starts — it could never
  match a Basic credential, so it would lock out every client including
  the operator.
- `mount --auth basic:user:token | bearer:token` for the client side.

Both remain OFF unless asked for: the default is still the isolated-network
diagnostic case, and defaulting to a self-signed certificate would train
operators to click through warnings.

### Streaming I/O
Files are transferred in 8 KB chunks, never fully buffered in RAM
(except for Range responses where the full file is read for slicing).
Progress tracking hooks into streaming callbacks.

## Build and Test

```bash
make                           # X64
make ARCH=aa64                 # AARCH64
scripts/test.sh                # Host-side tests (xfer-server.py, 25 tests)
scripts/test.sh --qemu         # Full suite with QEMU integration, X64 (119 tests)
scripts/test.sh --aarch64      # Also run the AARCH64 tests (implies --qemu)
```

The QEMU paths need `AXL_SDK_SRC` pointing at an axl-sdk source checkout;
`run-qemu.sh` ships in that tree, not in the packages.

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
*(Also done since this list was written: the WebDAV client for mount —
`src/mount/webfs-protocol-dav.c`, selected by `mount --protocol dav` or
auto-probed, so standard WebDAV servers mount without xfer-server.py; and
TLS, as `serve --tls` — see "TLS and Authentication" above.)*

- IPv6 support
