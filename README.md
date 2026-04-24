# HttpFS

[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

UEFI toolkit for bidirectional file transfer and remote filesystem
access. Build on your workstation, run immediately in the UEFI Shell —
no USB sticks, no BMC virtual media, no `.efi` shuffling.

Two commands:

- **`mount <url>`** — mount a workstation directory as a UEFI volume
  (FSn:). The UEFI Shell can read, write, and execute files in real time.
- **`serve`** — run an HTTP + WebDAV file server on the UEFI host,
  exposing local volumes to `curl`, browsers, or network drive mounts.

Plus `umount` and `list-nics`.

## Prerequisites

HttpFS is built with [AximCode's AXL SDK](https://github.com/aximcode/axl-sdk-releases)
(no EDK2). Install a prebuilt SDK package — this gives you `axl-cc`,
headers, and the UEFI target libs for x64 and aa64.

**Debian / Ubuntu:**

```bash
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.deb
sudo apt install ./axl-sdk.deb
```

**Fedora / RHEL:**

```bash
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.rpm
sudo dnf install ./axl-sdk.rpm
```

Packages install under `/usr` (headers, `/usr/bin/axl-cc`, UEFI libs).
HttpFS's `Makefile` picks them up with `AXL_SDK ?= /usr`. To build
against a local SDK checkout instead:

```bash
AXL_SDK=~/src/axl-sdk-releases/out make
```

## Build

```bash
make                 # Build HttpFS.efi and WebDavFsDxe.efi for x64
make ARCH=aa64       # AArch64
make clean
```

Output lands in `build/axl/<arch>/`.

## Quick Start

On your workstation, serve a directory:

```bash
./scripts/xfer-server.py --root /path/to/efi/tools
```

In the UEFI Shell, mount and use it:

```
FS0:\> HttpFS.efi mount http://10.0.0.5:8080/
FS0:\> ls fs1:
FS0:\> fs1:\IpmiTool.efi
```

Or run the server from UEFI and pull files from your workstation:

```
FS0:\> HttpFS.efi serve
```

```bash
curl http://<uefi-ip>:8080/fs0/
curl http://<uefi-ip>:8080/fs0/EFI/Boot/bootaa64.efi -o boot.efi
```

## Use Cases

- **Live development** — mount build output, run freshly compiled
  `.efi` files without manual transfer.
- **ARM64 server bootstrapping** — mount tools on ARM64 servers where
  virtual media is unreliable.
- **Log extraction** — `serve` lets you pull crash dumps, SMBIOS tables,
  or any file from the EFI System Partition via `curl`.
- **Bulk deployment** — upload or download entire directory trees.

## Architecture

| Component | Type | Description |
|-----------|------|-------------|
| `HttpFS.efi` | Application | CLI: `serve`, `mount`, `umount`, `list-nics` |
| `WebDavFsDxe.efi` | DXE driver | Mounts a remote directory as a UEFI volume via `EFI_FILE_PROTOCOL` over HTTP |

All HTTP, JSON, event loop, hash table, and network functionality comes
from the AXL SDK. See [docs/Design.md](docs/Design.md) for the full
design.

## Serve Options

```
HttpFS.efi serve [-p port] [-n nic] [-t timeout] [--read-only] [--write-only] [-v]
```

| Flag | Default | Description |
|------|---------|-------------|
| `-p` | 8080 | Listen port |
| `-n` | auto | NIC index (use `list-nics` to find) |
| `-t` | 0 | Idle timeout in seconds (0 = never) |
| `--read-only` | off | Block uploads and deletes |
| `--write-only` | off | Block downloads |
| `-v` | off | Verbose logging |

## Workstation File Server

`scripts/xfer-server.py` is the workstation-side companion for the
`mount` command. Python 3 stdlib only, no external dependencies.

```bash
./scripts/xfer-server.py                            # Serve current directory
./scripts/xfer-server.py --root /path --port 9090   # Custom root and port
./scripts/xfer-server.py --read-only                # Block uploads/deletes
```

## Testing

```bash
scripts/test.sh              # Host-side tests against xfer-server.py
scripts/test.sh --qemu       # Also run QEMU integration tests (X64)
scripts/test.sh --aarch64    # Include AARCH64 QEMU tests
```

QEMU tests use `run-qemu.sh`, which ships with the AXL SDK source
tree but not the `.deb`/`.rpm`. Point `AXL_SDK_SRC` at an
[axl-sdk-releases](https://github.com/aximcode/axl-sdk-releases)
checkout to enable them:

```bash
AXL_SDK_SRC=~/src/axl-sdk-releases scripts/test.sh --qemu
```

## Platform Notes

On ARM64 hardware (e.g. some ARM64 servers), firmware may not
auto-connect the network stack. HttpFS handles this by calling
`ConnectController` on SNP handles before NIC discovery. Use
`list-nics` to verify link status if networking isn't working.

## Built With

- [AXL SDK](https://github.com/aximcode/axl-sdk-releases) — HTTP, JSON,
  event loop, TCP, driver lifecycle, everything non-trivial.

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
