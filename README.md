# HttpFS

Bidirectional file transfer and remote filesystem access for UEFI.
Build on your workstation, run immediately in the UEFI Shell.

## Commands

- **`mount <url>`** — Mount a workstation directory as a UEFI volume (FSn:).
  The UEFI Shell can read, write, and execute files in real time.
- **`serve`** — Run an HTTP + WebDAV file server on the UEFI host,
  exposing local volumes to `curl`, browsers, or network drive mounts.
- **`umount`** — Unmount a previously mounted remote volume.
- **`list-nics`** — List network interfaces with MAC, link status, and IP.

## Quick Start

```bash
# Build (requires EDK2)
scripts/build.sh

# On your workstation — start the file server
scripts/xfer-server.py --root /path/to/efi/tools

# In the UEFI Shell — mount and use
HttpFS.efi mount http://10.0.0.5:8080/
ls fs1:
fs1:\IpmiTool.efi

# Or serve files from UEFI to your workstation
HttpFS.efi serve
# Then from workstation: curl http://<uefi-ip>:8080/fs0/
```

## Use Cases

- **Live development** — mount build output, run freshly compiled `.efi`
  files without manual transfer.
- **ARM64 server bootstrapping** — mount tools on ARM64 servers
  where BMC virtual media is unreliable.
- **Log extraction** — serve mode lets you pull crash dumps, SMBIOS
  tables, or any file from the EFI System Partition via `curl`.
- **Bulk deployment** — upload or download entire directory trees.

## Architecture

Two consumers backed by shared libraries:

| Component | Type | Description |
|-----------|------|-------------|
| HttpFS.efi | Application | CLI: `serve`, `mount`, `umount`, `list-nics` |
| WebDavFsDxe.efi | DXE Driver | Mounts remote directory as UEFI volume |
| FileTransferLib | Library | File I/O, streaming, SHA-256 |
| NetworkLib | Library | NIC discovery, DHCP, static IP, link detection |
| JsonLib | Library | Minimal JSON tokenizer |
| UdkLib (external) | Library | HTTP server, HTTP client, logging, data structures |

## Build

Requires EDK2 at `~/projects/edk2`.

```bash
scripts/build.sh                 # Both X64 and AARCH64
scripts/build.sh --arch X64      # X64 only
scripts/build.sh --arch AARCH64  # AARCH64 only
```

Output in `build/binaries/`:
- `HttpFS_X64.efi` / `HttpFS_AARCH64.efi`
- `WebDavFsDxe_X64.efi` / `WebDavFsDxe_AARCH64.efi`

## Testing

```bash
scripts/qemu.sh start    # Boot QEMU with port forwarding
scripts/test.sh           # Run all integration tests
scripts/test.sh --aarch64 # Include AARCH64 tests
```

## Workstation File Server

`xfer-server.py` is the workstation-side companion for the `mount` command.
No external dependencies — Python 3 stdlib only.

```bash
./scripts/xfer-server.py                          # Serve current directory
./scripts/xfer-server.py --root /path --port 9090 # Custom root and port
./scripts/xfer-server.py --read-only              # Block uploads/deletes
```

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

## Platform Notes

On some ARM64 server hardware, firmware may not auto-connect
the network stack. HttpFS handles this by calling `ConnectController`
on SNP handles before NIC discovery. Use `list-nics` to verify link
status if networking isn't working. See [docs/Design.md](docs/Design.md)
for details.

## License

BSD-2-Clause-Patent
