#!/usr/bin/env python3
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
"""
long-name-server -- test fixture for the mount driver's name-length rule.

Serves ONE synthetic directory listing, in the JSON protocol (GET /info,
GET /list/) or, with --webdav, as WebDAV (OPTIONS, PROPFIND). The
listing holds names no real host filesystem can: Linux caps a name at
255 bytes, and the mount's rule (WEBFS_NAME_SIZE) is about names longer
than that, which a FAT or NTFS server stores as up to 255 UTF-16 units.

Entries:
  readme.txt               an ordinary file
  FIT_NAME  (255 bytes)    fits; must be listed exactly once
  LONG_NAME (256 bytes)    does not fit; FIT_NAME is its 255-byte prefix,
                           so a mount that CUTS names has two entries
                           named FIT_NAME. Sized 6 bytes against FIT_NAME's
                           5, so the listed one shows which it is
  HREF_NAME (255 bytes)    WebDAV only: no displayname, and an href with
                           every byte percent-encoded (765 characters), so
                           a client that cuts the ENCODED href shows a
                           shortened name

Usage:
    ./long-name-server.py --port 9877 [--webdav] [--bind 0.0.0.0]
"""

from __future__ import annotations

import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import quote, urlparse

FIT_NAME = "LONGNAME-" + "a" * 246
LONG_NAME = "LONGNAME-" + "a" * 247
HREF_NAME = "HREFNAME-" + "b" * 246


def _every_byte_encoded(name: str) -> str:
    """Percent-encode every byte of @p name -- legal, if unusual, in an href."""
    return "".join(f"%{b:02X}" for b in name.encode("utf-8"))


def _response(href: str, displayname: str | None, size: int | None) -> str:
    """One <D:response> of a PROPFIND multistatus; @p size None is a collection."""
    rtype = "<D:collection/>" if size is None else ""
    disp = "" if displayname is None else f"<D:displayname>{displayname}</D:displayname>"
    length = "" if size is None else f"<D:getcontentlength>{size}</D:getcontentlength>"
    return (f"<D:response><D:href>{href}</D:href><D:propstat><D:prop>"
            f"<D:resourcetype>{rtype}</D:resourcetype>{disp}{length}"
            f"</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
            f"</D:response>")


class LongNameHandler(BaseHTTPRequestHandler):
    """Answers the handful of requests a mount's listing needs."""

    webdav: bool = False

    def _send(self, status: int, body: bytes, ctype: str,
              extra: dict[str, str] | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _root(self) -> bool:
        path = urlparse(self.path).path
        return path in ("/", "/list", "/list/")

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if not self.webdav and path == "/info":
            body = json.dumps({"version": "1.0", "root": "/", "read_only": True})
            self._send(200, body.encode(), "application/json")
        elif not self.webdav and self._root():
            entries: list[dict[str, object]] = [
                {"name": name, "size": size, "dir": False,
                 "modified": "2026-01-01T00:00:00Z"}
                for name, size in (("readme.txt", 5), (FIT_NAME, 5),
                                   (LONG_NAME, 6))
            ]
            self._send(200, json.dumps(entries).encode(), "application/json")
        else:
            self._send(404, b"not found\n", "text/plain")

    def do_OPTIONS(self) -> None:
        extra = {"Allow": "OPTIONS, GET, PROPFIND"}
        if self.webdav:
            extra["DAV"] = "1"
        self._send(200, b"", "text/plain", extra)

    def do_PROPFIND(self) -> None:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length > 0:
            self.rfile.read(length)
        if not self.webdav or urlparse(self.path).path != "/":
            self._send(404, b"not found\n", "text/plain")
            return
        parts = [_response("/", "", None)]
        if self.headers.get("Depth", "1") != "0":
            parts.append(_response("/readme.txt", "readme.txt", 5))
            parts.append(_response("/" + quote(FIT_NAME), FIT_NAME, 5))
            parts.append(_response("/" + quote(LONG_NAME), LONG_NAME, 6))
            parts.append(_response("/" + _every_byte_encoded(HREF_NAME), None, 5))
        body = ('<?xml version="1.0" encoding="utf-8"?>'
                '<D:multistatus xmlns:D="DAV:">' + "".join(parts)
                + "</D:multistatus>")
        self._send(207, body.encode(), 'application/xml; charset="utf-8"')

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002
        print(f"  {self.address_string()} {format % args}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Serve a synthetic listing of over-long names.")
    parser.add_argument("--port", type=int, default=9877)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--webdav", action="store_true",
                        help="speak WebDAV instead of the JSON protocol")
    args = parser.parse_args()
    LongNameHandler.webdav = args.webdav
    server = HTTPServer((args.bind, args.port), LongNameHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()


if __name__ == "__main__":
    main()
