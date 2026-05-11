#!/usr/bin/env python3
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
"""
xfer-server — Workstation file server for axl-webfs mount command.

Default mode serves a local directory over HTTP with JSON directory
listings (bespoke protocol; Python stdlib only). The --webdav flag
swaps the implementation for an RFC 4918 WebDAV server (wsgidav +
cheroot), so axl-webfs's mount client can either keep talking the
JSON protocol or migrate to standard WebDAV — both target the same
on-disk root.

Usage:
    ./xfer-server.py                        # JSON protocol, current dir
    ./xfer-server.py --root /path --port 9876
    ./xfer-server.py --read-only
    ./xfer-server.py --webdav               # WebDAV protocol (needs wsgidav)
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import unquote, urlparse, parse_qs

VERSION = "1.0"

# Default credential file. Looked up automatically when neither
# --basic-auth nor --basic-auth-file is given on the command line.
# Path follows the XDG Base Dir spec for user config; falls back to
# ~/.config/axl-webfs/auth on systems without XDG_CONFIG_HOME.
def _default_auth_file_path() -> str:
    cfg = os.environ.get("XDG_CONFIG_HOME") \
        or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(cfg, "axl-webfs", "auth")


class XferServer(HTTPServer):
    """HTTPServer subclass with typed attributes for root_dir and read_only."""

    root_dir: str
    read_only: bool
    digest_cache: dict[str, tuple[int, float, str]]
    basic_auth: str | None   # "user:password" or None to disable

    def __init__(self, server_address: tuple[str, int],
                 handler: type[BaseHTTPRequestHandler],
                 root_dir: str, read_only: bool,
                 basic_auth: str | None = None) -> None:
        super().__init__(server_address, handler)
        self.root_dir = root_dir
        self.read_only = read_only
        self.basic_auth = basic_auth
        # Per-(path,size,mtime) SHA-256 cache so repeated GETs and
        # HEAD-then-GET chains don't re-hash multi-hundred-MB files.
        self.digest_cache = {}


def _file_sha256(server: XferServer, local_path: str) -> str | None:
    """Compute or recall the SHA-256 of @p local_path. Returns 64
    lowercase hex chars on success, None on read error.
    Cache key includes file size and mtime so an in-place edit
    invalidates the entry promptly."""
    try:
        st = os.stat(local_path)
    except OSError:
        return None
    key = local_path
    cached = server.digest_cache.get(key)
    if cached is not None and cached[0] == st.st_size and cached[1] == st.st_mtime:
        return cached[2]
    h = hashlib.sha256()
    try:
        with open(local_path, "rb") as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                h.update(chunk)
    except OSError:
        return None
    hex_value = h.hexdigest()
    server.digest_cache[key] = (st.st_size, st.st_mtime, hex_value)
    return hex_value


class XferHandler(BaseHTTPRequestHandler):
    """HTTP request handler for axl-webfs mount protocol."""

    server_version = f"xfer-server/{VERSION}"
    # HTTP/1.1 + keep-alive. The default BaseHTTPRequestHandler runs
    # HTTP/1.0, which forces Connection: close on every response — and
    # the mount client reads files in 64 KB Range chunks, so a 500 MB
    # download would otherwise pay the TCP-handshake cost ~8000 times.
    # Enabling HTTP/1.1 lets the stdlib emit a Content-Length-framed
    # persistent connection, dropping per-request setup by an order of
    # magnitude in practice.
    protocol_version = "HTTP/1.1"

    @property
    def xfer_server(self) -> XferServer:
        assert isinstance(self.server, XferServer)
        return self.server

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002
        """Override to use cleaner log format."""
        sys.stderr.write(f"  {self.address_string()} {format % args}\n")

    def _check_auth(self) -> bool:
        """Validate Authorization: Basic against the configured creds.

        Returns True when the request is allowed to proceed. On
        failure, emits a 401 with WWW-Authenticate and returns False,
        so handlers should `if not self._check_auth(): return`.

        No-op when --basic-auth was not set."""
        creds = self.xfer_server.basic_auth
        if creds is None:
            return True
        hdr = self.headers.get("Authorization", "")
        if hdr.startswith("Basic "):
            try:
                got = base64.b64decode(hdr[6:]).decode("utf-8")
            except (ValueError, UnicodeDecodeError):
                got = ""
            if got == creds:
                return True
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="xfer-server"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def _resolve_path(self, url_path: str) -> str | None:
        """Resolve URL path to local filesystem path, preventing traversal."""
        clean = unquote(url_path).replace("\\", "/")
        # Strip leading prefix (/files/ or /list/)
        for prefix in ("/files/", "/list/"):
            if clean.startswith(prefix):
                clean = clean[len(prefix):]
                break
        # Prevent directory traversal
        parts = [p for p in clean.split("/") if p and p != ".."]
        local = os.path.join(self.xfer_server.root_dir, *parts)
        real = os.path.realpath(local)
        root_real = os.path.realpath(self.xfer_server.root_dir)
        if not real.startswith(root_real):
            return None
        return real

    def _send_json(self, data: object, status: int = 200) -> None:
        body = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _send_error(self, code: int, msg: str) -> None:
        body = msg.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if not self._check_auth():
            return
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/")

        # GET /info — server metadata
        if path == "/info":
            self._send_json({
                "version": VERSION,
                "root": self.xfer_server.root_dir,
                "read_only": self.xfer_server.read_only,
            })
            return

        # GET /list/<path> — JSON directory listing
        if path.startswith("/list"):
            local = self._resolve_path(self.path)
            if local is None:
                self._send_error(403, "Forbidden")
                return
            if not os.path.isdir(local):
                self._send_error(404, "Not a directory")
                return

            entries: list[dict[str, object]] = []
            for name in sorted(os.listdir(local)):
                full = os.path.join(local, name)
                try:
                    st = os.stat(full)
                    entries.append({
                        "name": name,
                        "size": st.st_size if not os.path.isdir(full) else 0,
                        "dir": os.path.isdir(full),
                        "modified": datetime.datetime.fromtimestamp(
                            st.st_mtime, tz=datetime.timezone.utc
                        ).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    })
                except OSError:
                    continue

            self._send_json(entries)
            return

        # GET /files/<path> — download file (with Range support)
        if path.startswith("/files"):
            self._serve_file(head_only=False)
            return

        self._send_error(404, "Unknown endpoint")

    def do_HEAD(self) -> None:
        """HEAD on a file path: emit the headers a GET would, with
        no body. Mount clients use this to fetch the file's SHA-256
        from the Digest header before reading via Range."""
        if not self._check_auth():
            return
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/")
        if path.startswith("/files"):
            self._serve_file(head_only=True)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def _serve_file(self, head_only: bool) -> None:
        """Shared GET/HEAD body. Computes the file's SHA-256 (cached
        per (size, mtime)) and emits `Digest: sha-256=<hex>` so the
        mount client can verify integrity end-to-end without an out-
        of-band hash exchange."""
        local = self._resolve_path(self.path)
        if local is None:
            self._send_error(403, "Forbidden")
            return
        if not os.path.isfile(local):
            self._send_error(404, "Not found")
            return

        file_size = os.path.getsize(local)
        range_hdr = self.headers.get("Range")
        start = 0
        end = file_size - 1

        if range_hdr and range_hdr.startswith("bytes="):
            range_spec = range_hdr[6:]
            if "-" in range_spec:
                parts = range_spec.split("-", 1)
                if parts[0]:
                    start = int(parts[0])
                if parts[1]:
                    end = int(parts[1])

            if start > end or start >= file_size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{file_size}")
                self.end_headers()
                return

            length = end - start + 1
            self.send_response(206)
            self.send_header("Content-Range",
                             f"bytes {start}-{end}/{file_size}")
        else:
            length = file_size
            self.send_response(200)

        # RFC 3230 Digest header — opt-in via `Want-Digest:`. Full-
        # file hash (not range-slice) so HEAD and Range GET responses
        # carry the same value; mount clients send Want-Digest on
        # their first Range read and validate incrementally. curl /
        # browser GETs without the request header skip the compute
        # (~30 ms per MB on this machine).
        want_digest = (self.headers.get("Want-Digest") or "").lower()
        if "sha-256" in want_digest or "sha256" in want_digest:
            digest_hex = _file_sha256(self.xfer_server, local)
            if digest_hex is not None:
                self.send_header("Digest", f"sha-256={digest_hex}")

        self.send_header("Content-Length", str(length))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        if head_only:
            return

        with open(local, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def do_PUT(self) -> None:
        if not self._check_auth():
            return
        if self.xfer_server.read_only:
            self._send_error(403, "Server is read-only")
            return

        if not self.path.startswith("/files/"):
            self._send_error(404, "Unknown endpoint")
            return

        local = self._resolve_path(self.path)
        if local is None:
            self._send_error(403, "Forbidden")
            return

        # Create parent directories
        parent = os.path.dirname(local)
        os.makedirs(parent, exist_ok=True)

        transfer_encoding = self.headers.get("Transfer-Encoding", "")
        content_length = self.headers.get("Content-Length")

        with open(local, "wb") as f:
            if "chunked" in transfer_encoding.lower():
                # Read chunked transfer encoding
                while True:
                    line = self.rfile.readline().strip()
                    chunk_size = int(line, 16)
                    if chunk_size == 0:
                        self.rfile.readline()  # trailing CRLF
                        break
                    data = self.rfile.read(chunk_size)
                    f.write(data)
                    self.rfile.readline()  # chunk-terminating CRLF
            elif content_length is not None:
                remaining = int(content_length)
                while remaining > 0:
                    chunk = self.rfile.read(min(65536, remaining))
                    if not chunk:
                        break
                    f.write(chunk)
                    remaining -= len(chunk)
            else:
                # No Content-Length, no chunked — read until connection
                # closes (common with curl -T -)
                # Use a short timeout approach: try reading in chunks
                import select
                while True:
                    ready, _, _ = select.select([self.rfile], [], [], 1.0)
                    if not ready:
                        break
                    chunk = self.rfile.read(65536)
                    if not chunk:
                        break
                    f.write(chunk)

        self.send_response(201)
        self.send_header("Content-Type", "text/plain")
        body = f"Created: {os.path.basename(local)}\n".encode("utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_DELETE(self) -> None:
        if not self._check_auth():
            return
        if self.xfer_server.read_only:
            self._send_error(403, "Server is read-only")
            return

        if not self.path.startswith("/files/"):
            self._send_error(404, "Unknown endpoint")
            return

        local = self._resolve_path(self.path)
        if local is None:
            self._send_error(403, "Forbidden")
            return

        if not os.path.exists(local):
            self._send_error(404, "Not found")
            return

        try:
            if os.path.isdir(local):
                os.rmdir(local)
            else:
                os.remove(local)
        except OSError as e:
            self._send_error(409, str(e))
            return

        self._send_json({"deleted": os.path.basename(local)})

    def do_POST(self) -> None:
        if not self._check_auth():
            return
        if self.xfer_server.read_only:
            self._send_error(403, "Server is read-only")
            return

        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)

        if "mkdir" in qs or parsed.query == "mkdir":
            local = self._resolve_path(parsed.path)
            if local is None:
                self._send_error(403, "Forbidden")
                return
            os.makedirs(local, exist_ok=True)
            self._send_json({"created": os.path.basename(local)}, 201)
            return

        # POST /files/<path>?rename=<newpath> — rename/move file or directory
        if "rename" in qs:
            src = self._resolve_path(parsed.path)
            if src is None:
                self._send_error(403, "Forbidden")
                return
            if not os.path.exists(src):
                self._send_error(404, "Source not found")
                return

            new_name = qs["rename"][0]
            # new_name can be a relative name or absolute path within root
            if "/" in new_name or "\\" in new_name:
                # Absolute path — resolve it
                dst = self._resolve_path("/files/" + new_name)
            else:
                # Just a filename — same directory
                dst = os.path.join(os.path.dirname(src), new_name)
                # Verify it stays within root
                root_real = os.path.realpath(self.xfer_server.root_dir)
                if not os.path.realpath(dst).startswith(root_real):
                    self._send_error(403, "Forbidden")
                    return

            if dst is None:
                self._send_error(403, "Forbidden")
                return

            try:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.rename(src, dst)
            except OSError as e:
                self._send_error(409, str(e))
                return

            self._send_json({"renamed": os.path.basename(dst)})
            return

        self._send_error(400, "Unknown POST action")


def run_webdav(root: str, bind: str, port: int, read_only: bool) -> None:
    """Serve @p root over RFC 4918 WebDAV via wsgidav + cheroot.

    Anonymous access (`simple_dc.anonymous`) so the mount client
    doesn't need credentials in the URL; mount mode currently runs
    on trusted networks where the JSON variant is also wide open.
    Auth can be layered on later by swapping out simple_dc.
    """
    try:
        from wsgidav.wsgidav_app import WsgiDAVApp
        from cheroot import wsgi as cheroot_wsgi
    except ImportError as exc:
        print(f"Error: --webdav requires wsgidav + cheroot ({exc}).\n"
              f"Install with: pip install --user wsgidav cheroot",
              file=sys.stderr)
        sys.exit(2)

    config = {
        "host": bind,
        "port": port,
        "provider_mapping": {"/": root},
        "simple_dc": {"user_mapping": {"*": True}},  # anonymous access
        "verbose": 1,
        "logging": {"enable_loggers": []},
        "lock_storage": True,
        "dir_browser": {"enable": True},
        "suppress_version_info": True,
    }
    if read_only:
        # wsgidav exposes per-share read-only via the provider config.
        # Re-wire provider_mapping with a readonly flag.
        config["provider_mapping"] = {"/": {"root": root, "readonly": True}}

    app = WsgiDAVApp(config)
    server = cheroot_wsgi.Server((bind, port), app)
    try:
        server.start()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.stop()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="xfer-server — Workstation file server for axl-webfs")
    parser.add_argument("--root", default=".",
                        help="Directory to serve (default: current)")
    parser.add_argument("--port", type=int, default=9876,
                        help="Listen port (default: 9876, matches "
                             "the UEFI mount client's DEFAULT_SERVER_PORT)")
    parser.add_argument("--bind", default="0.0.0.0",
                        help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--read-only", action="store_true",
                        help="Disable uploads and deletes")
    parser.add_argument("--webdav", action="store_true",
                        help="Speak RFC 4918 WebDAV instead of the "
                             "bespoke JSON protocol (requires wsgidav).")
    auth_group = parser.add_mutually_exclusive_group()
    auth_group.add_argument("--basic-auth", default=None,
                            metavar="USER:PASSWORD",
                            help="Require HTTP Basic Auth. "
                                 "Convenience for dev; the credential lands "
                                 "in shell history and `ps` output — prefer "
                                 "--basic-auth-file for routine use.")
    auth_group.add_argument("--basic-auth-file", default=None,
                            metavar="PATH",
                            help="Read USER:PASSWORD from PATH (single line, "
                                 "trailing newline stripped). When neither "
                                 "this nor --basic-auth is given the server "
                                 "auto-loads $XDG_CONFIG_HOME/axl-webfs/auth "
                                 "(default ~/.config/axl-webfs/auth) if it "
                                 "exists; otherwise the server runs without "
                                 "authentication and warns at startup.")
    parser.add_argument("--no-auth", action="store_true",
                        help="Skip the default auth-file lookup and run "
                             "anonymous without a warning. Use when you "
                             "explicitly want a public-read share on a "
                             "trusted network.")
    args = parser.parse_args()

    root = os.path.realpath(args.root)
    if not os.path.isdir(root):
        print(f"Error: {root} is not a directory", file=sys.stderr)
        sys.exit(1)

    # Resolve credentials with explicit > file-arg > default-file >
    # nothing precedence. A bad explicit file is fatal; the default
    # file path missing is just silently treated as "no auth" (and
    # surfaced via the no-auth warning further down). argparse's mutex
    # group keeps at most one of the explicit flags set.
    basic_auth = args.basic_auth
    auth_source = "--basic-auth" if basic_auth else None
    auth_file_used: str | None = None

    if args.basic_auth_file is not None:
        # Explicit file: fail loudly if unreadable.
        try:
            with open(args.basic_auth_file, encoding="utf-8") as f:
                basic_auth = f.read().strip()
        except OSError as e:
            print(f"Error reading --basic-auth-file: {e}", file=sys.stderr)
            sys.exit(1)
        auth_file_used = args.basic_auth_file
        auth_source = f"--basic-auth-file {args.basic_auth_file}"
    elif basic_auth is None and not args.no_auth:
        # Implicit lookup: try the default config path. Missing file is
        # not an error — it just means "fall through to no-auth and
        # warn".
        default_path = _default_auth_file_path()
        if os.path.isfile(default_path):
            try:
                with open(default_path, encoding="utf-8") as f:
                    basic_auth = f.read().strip()
                auth_file_used = default_path
                auth_source = f"default file {default_path}"
            except OSError as e:
                print(f"Warning: default auth file {default_path} "
                      f"is present but unreadable: {e}", file=sys.stderr)

    if basic_auth is not None and ":" not in basic_auth:
        src = auth_file_used or "--basic-auth"
        print(f"Error: {src} must contain USER:PASSWORD",
              file=sys.stderr)
        sys.exit(1)

    # World-readable credential files are a footgun. curl prints a
    # similar warning for --netrc-file; do the same.
    if auth_file_used is not None:
        try:
            file_mode = os.stat(auth_file_used).st_mode
            if file_mode & 0o077:
                print(f"Warning: {auth_file_used} is accessible to "
                      f"other users (mode {file_mode & 0o777:o}); restrict "
                      f"with `chmod 600`.", file=sys.stderr)
        except OSError:
            pass

    mode = "read-only" if args.read_only else "read-write"
    proto = "WebDAV" if args.webdav else "JSON"

    # Build the user-visible access URL. Pick an instructive host: if
    # binding to a wildcard interface (0.0.0.0 / ::), substitute a
    # concrete reachable address — try the primary outbound IP, fall
    # back to the hostname, fall back to the literal bind.
    display_host = args.bind
    if args.bind in ("0.0.0.0", "::", ""):
        try:
            # No traffic actually sent — UDP connect(2) on a non-
            # routable address picks the OS's preferred source IP.
            import socket
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.connect(("10.255.255.255", 1))
                display_host = s.getsockname()[0]
            finally:
                s.close()
        except OSError:
            try:
                import socket
                display_host = socket.gethostname()
            except OSError:
                display_host = "localhost"

    access_url = f"http://{display_host}:{args.port}/"

    auth_note = " (Basic Auth required)" if basic_auth else ""
    print(f"xfer-server v{VERSION} ({proto}){auth_note}")
    print(f"Serving {root}")
    print(f"  URL:   {access_url}")
    print(f"  Mode:  {mode}")
    if basic_auth is not None:
        # Surface the username so the user knows who they're auth'ing
        # as without having to grep the credential file. Password is
        # never displayed. Split on the first ':' so passwords
        # containing colons stay intact even though we don't print
        # them.
        auth_user = basic_auth.split(":", 1)[0]
        print(f'  Auth:  Basic — user "{auth_user}" ({auth_source})')
    elif args.no_auth:
        print("  Auth:  none (--no-auth)")
    else:
        # Be loud about no-auth so trusted-network assumptions are
        # visible. The user can suppress with --no-auth if intentional.
        print("  Auth:  ANONYMOUS — anyone reachable on the network "
              "can read/write")
        print(f"         Drop USER:PASSWORD into "
              f"{_default_auth_file_path()} (chmod 600) to require auth, "
              f"or pass --no-auth to silence this message.")
    print("Ready for axl-webfs mount connections.")
    print("Press Ctrl-C to stop.\n")

    if args.webdav:
        if basic_auth is not None:
            print("Note: --webdav uses wsgidav's anonymous auth provider; "
                  "Basic-Auth gating applies only to the JSON path. "
                  "For authenticated WebDAV use the JSON path + a WebDAV "
                  "client that speaks both, or configure wsgidav directly.",
                  file=sys.stderr)
        run_webdav(root, args.bind, args.port, args.read_only)
        return

    server = XferServer((args.bind, args.port), XferHandler, root,
                        args.read_only, basic_auth)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
