#!/usr/bin/env python3
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
"""
xfer-server — Workstation file server for axl-webfs mount command.

Serves a local directory over HTTP with JSON directory listings.
No external dependencies — Python 3 stdlib only.

Usage:
    ./xfer-server.py                        # Serve current directory
    ./xfer-server.py --root /path --port 9090
    ./xfer-server.py --read-only
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import unquote, urlparse, parse_qs

VERSION = "1.0"


class XferServer(HTTPServer):
    """HTTPServer subclass with typed attributes for root_dir and read_only."""

    root_dir: str
    read_only: bool

    def __init__(self, server_address: tuple[str, int],
                 handler: type[BaseHTTPRequestHandler],
                 root_dir: str, read_only: bool) -> None:
        super().__init__(server_address, handler)
        self.root_dir = root_dir
        self.read_only = read_only


class XferHandler(BaseHTTPRequestHandler):
    """HTTP request handler for axl-webfs mount protocol."""

    server_version = f"xfer-server/{VERSION}"

    @property
    def xfer_server(self) -> XferServer:
        assert isinstance(self.server, XferServer)
        return self.server

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002
        """Override to use cleaner log format."""
        sys.stderr.write(f"  {self.address_string()} {format % args}\n")

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

            self.send_header("Content-Length", str(length))
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            with open(local, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(65536, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            return

        self._send_error(404, "Unknown endpoint")

    def do_PUT(self) -> None:
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


def main() -> None:
    parser = argparse.ArgumentParser(
        description="xfer-server — Workstation file server for axl-webfs")
    parser.add_argument("--root", default=".",
                        help="Directory to serve (default: current)")
    parser.add_argument("--port", type=int, default=8080,
                        help="Listen port (default: 8080)")
    parser.add_argument("--bind", default="0.0.0.0",
                        help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--read-only", action="store_true",
                        help="Disable uploads and deletes")
    args = parser.parse_args()

    root = os.path.realpath(args.root)
    if not os.path.isdir(root):
        print(f"Error: {root} is not a directory", file=sys.stderr)
        sys.exit(1)

    server = XferServer((args.bind, args.port), XferHandler, root, args.read_only)

    mode = "read-only" if args.read_only else "read-write"
    print(f"xfer-server v{VERSION}")
    print(f"Serving {root} on {args.bind}:{args.port}")
    print(f"Mode: {mode}")
    print("Ready for axl-webfs mount connections.")
    print("Press Ctrl-C to stop.\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
