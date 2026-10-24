#!/usr/bin/env python3
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
"""
listing-server -- test fixture serving synthetic trees to the mount driver.

Serves one in-memory tree in the JSON protocol (GET /info, /list/<dir>,
/files/<path>) or, with --webdav, as WebDAV (OPTIONS, PROPFIND, GET).
Each --case builds a tree no real host directory can stand in for:

  long-names  Names past Linux's 255-byte limit (WEBFS_NAME_SIZE):
              FIT_NAME (255 bytes, fits) and LONG_NAME (256 bytes, whose
              255-byte cut IS FIT_NAME; sized 6 against FIT_NAME's 5 so a
              listing shows which one it holds). Over WebDAV also
              HREF_NAME: 255 bytes, no displayname, every byte of its
              href percent-encoded (765 characters).
  entities    WebDAV displaynames that need decoding: one spelled with
              `&amp;`/`&lt;`/`&gt;`, one split by a CDATA section into
              three text tokens. And names a client must encode: "what?.txt"
              (a '?' is a name byte, not a query) and a directory
              "pct%41dir" (a '%' that must not be decoded again). Over
              WebDAV a MOVE renames an entry, logging the decoded path of
              its Destination ("MOVE-DEST <path>"), so a rename in
              "sp ace" shows how the client encoded that directory.
  big         150 files: a listing well past 4 KB and past 64 entries.
  huge        16385 files: one past the mount's WEBFS_LIST_MAX_ENTRIES.
  relaxed     JSON only: a root listing with a trailing comma -- valid
              JSON5, not RFC 8259 -- which a strict client refuses.
  based       WebDAV, served under --base: a root whose PROPFIND carries
              hrefs a client must normalize -- relative ("rel.txt"), with
              a "." segment ("<base>/./dot.txt"), one outside the share
              ("/elsewhere/foreign.txt"), one climbing out of it
              ("<base>/../evil.txt"), and one that is no child of the
              listed directory ("<base>/sp ace/a.txt" in the root) -- and
              a directory "sp ace" to rename in. Only dot.txt, readme.txt,
              rel.txt and "sp ace" are entries of the root.
  deep        A file whose path is 505 bytes -- "/files" + it is a 511-byte
              request target -- and a sibling whose path is 511 and
              starts with those 505 bytes: a client that cut
              "/files<path>" at 511 would read the first file when asked
              for the second. A third, 520 bytes, is DEEP_LONG plus 9: a
              path cut at 511 bytes anywhere reads DEEP_LONG instead.

--base PATH serves the whole tree under PATH (decoded, e.g. "/my share"):
requests must name it, hrefs carry it ("{B}" in an Entry's href is the
encoded base), and a MOVE's logged Destination includes it. --no-self
leaves the queried collection out of every PROPFIND.

Usage:
    ./listing-server.py --case long-names --port 9877 [--webdav]
        [--base PATH] [--no-self]
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import quote, unquote, urlparse
from xml.sax.saxutils import escape

FIT_NAME = "LONGNAME-" + "a" * 246
LONG_NAME = "LONGNAME-" + "a" * 247
HREF_NAME = "HREFNAME-" + "b" * 246
DEEP_DIR1 = "d" * 250
DEEP_DIR2 = "e" * 250
DEEP_SHORT = "f" * 2               # /DEEP_DIR1/DEEP_DIR2/ff: 505 bytes
DEEP_LONG = "f" * 2 + "g" * 6      # same plus 6 bytes: 511
DEEP_OVER = "f" * 2 + "g" * 15     # 520 bytes: its 511-byte cut IS DEEP_LONG


@dataclass
class Entry:
    """One child of a directory in the served tree."""

    name: str
    content: bytes | None = None     # None: a directory
    displayname: str | None = None   # raw XML for <D:displayname>; None: escaped name
    href: str | None = None          # raw <D:href>; None: the quoted path
    dav_only: bool = False


_URI_CHARS = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    "-._~:/?#[]@!$&'()*+,;=%")


def _valid_uri(uri: str) -> bool:
    """Whether @p uri uses only RFC 3986 characters, every '%' starting an escape."""
    if not uri or any(c not in _URI_CHARS for c in uri):
        return False
    hexdigits = "0123456789abcdefABCDEF"
    for i, c in enumerate(uri):
        if c == "%" and (i + 2 >= len(uri) or uri[i + 1] not in hexdigits
                         or uri[i + 2] not in hexdigits):
            return False
    return True


def _every_byte_encoded(name: str) -> str:
    """Percent-encode every byte of @p name -- legal, if unusual, in an href."""
    return "".join(f"%{b:02X}" for b in name.encode("utf-8"))


def build_tree(case: str) -> dict[str, list[Entry]]:
    """Directory path ("/" or "/a/b") -> its children, for @p case."""
    readme = Entry("readme.txt", b"hello")
    if case == "long-names":
        return {"/": [
            readme,
            Entry(FIT_NAME, b"fit!\n"),
            Entry(LONG_NAME, b"long!\n"),
            Entry(HREF_NAME, b"href\n", displayname="",
                  href="/" + _every_byte_encoded(HREF_NAME), dav_only=True),
        ]}
    if case == "entities":
        return {"/": [
            readme,
            Entry("R&D <notes>.txt", b"notes"),
            Entry("part1&part2.txt", b"parts",
                  displayname="part1<![CDATA[&part2]]>.txt"),
            Entry("what?.txt", b"question\n"),
            Entry("pct%41dir"),
            Entry("sp ace"),
        ], "/pct%41dir": [Entry("inner.txt", b"inner")],
           "/sp ace": [Entry("a.txt", b"rename me")]}
    if case == "relaxed":
        return {"/": [readme]}
    if case == "based":
        return {"/": [
            readme,
            Entry("rel.txt", b"rel", href="rel.txt"),
            Entry("dot.txt", b"dot", href="{B}/./dot.txt"),
            Entry("foreign.txt", b"x", href="/elsewhere/foreign.txt"),
            Entry("evil.txt", b"x", href="{B}/../evil.txt"),
            Entry("a.txt", b"x", href="{B}/sp%20ace/a.txt"),
            Entry("sp ace"),
        ], "/sp ace": [Entry("a.txt", b"rename me")]}
    if case == "huge":
        return {"/": [Entry(f"h-{i:05d}.txt", b"h") for i in range(16385)]}
    if case == "big":
        files = [Entry(f"big-{i:03d}-{'p' * 40}.txt", b"big!\n") for i in range(150)]
        return {"/": [readme, *files]}
    if case == "deep":
        return {
            "/": [readme, Entry(DEEP_DIR1)],
            f"/{DEEP_DIR1}": [Entry(DEEP_DIR2)],
            f"/{DEEP_DIR1}/{DEEP_DIR2}": [
                Entry(DEEP_SHORT, b"short-file\n"),
                Entry(DEEP_LONG, b"long-file\n"),
                Entry(DEEP_OVER, b"over-file\n"),
            ],
        }
    raise ValueError(f"unknown case {case!r}")


class ListingHandler(BaseHTTPRequestHandler):
    """Answers the requests a mount makes to list and read the tree."""

    webdav: bool = False
    tree: dict[str, list[Entry]] = {}
    case: str = ""
    base: str = ""          # decoded share prefix, "" for the root
    no_self: bool = False

    def _local(self, url_path: str) -> str | None:
        """The tree path @p url_path (decoded) names under the base, or None."""
        if not self.base:
            return url_path
        if url_path == self.base or url_path.startswith(self.base + "/"):
            return url_path[len(self.base):] or "/"
        return None

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

    def _entries(self, path: str) -> list[Entry] | None:
        key = path.rstrip("/") or "/"
        found = self.tree.get(key)
        if found is None:
            return None
        return [e for e in found if self.webdav or not e.dav_only]

    def _lookup(self, path: str) -> Entry | None:
        parent, _, name = path.rstrip("/").rpartition("/")
        for entry in self._entries(parent or "/") or []:
            if entry.name == name:
                return entry
        return None

    def _send_file(self, path: str) -> None:
        entry = self._lookup(path)
        if entry is None or entry.content is None:
            self._send(404, b"not found\n", "text/plain")
            return
        data = entry.content
        rng = self.headers.get("Range")
        if rng is not None and rng.startswith("bytes="):
            first, _, last = rng[6:].partition("-")
            start = int(first)
            end = min(int(last) if last else len(data) - 1, len(data) - 1)
            body = data[start:end + 1]
            self._send(206, body, "application/octet-stream",
                       {"Content-Range": f"bytes {start}-{end}/{len(data)}"})
            return
        self._send(200, data, "application/octet-stream")

    def do_GET(self) -> None:
        local = self._local(unquote(urlparse(self.path).path))
        if local is None:
            self._send(404, b"not found\n", "text/plain")
            return
        path = local
        if self.webdav:
            self._send_file(path)
        elif path == "/info":
            body = json.dumps({"version": "1.0", "root": "/", "read_only": True})
            self._send(200, body.encode(), "application/json")
        elif self.case == "relaxed" and path in ("/list", "/list/"):
            body = ('[{"name": "readme.txt", "size": 5, "dir": false, '
                    '"modified": "2026-01-01T00:00:00Z"},]')
            self._send(200, body.encode(), "application/json")
        elif path == "/list" or path.startswith("/list/"):
            entries = self._entries(path[len("/list"):] or "/")
            if entries is None:
                self._send(404, b"not a directory\n", "text/plain")
                return
            listing: list[dict[str, object]] = [
                {"name": e.name, "size": len(e.content or b""),
                 "dir": e.content is None, "modified": "2026-01-01T00:00:00Z"}
                for e in entries
            ]
            self._send(200, json.dumps(listing).encode(), "application/json")
        elif path.startswith("/files/"):
            self._send_file(path[len("/files"):])
        else:
            self._send(404, b"not found\n", "text/plain")

    def do_HEAD(self) -> None:
        self.do_GET()

    def do_MOVE(self) -> None:
        raw = self.headers.get("Destination", "")
        if not _valid_uri(raw):
            # A space, a non-ASCII byte or a bare '%' is no URI (RFC
            # 3986): a strict server refuses it, as real ones do.
            print(f"  MOVE-DEST-INVALID {raw!r}", flush=True)
            self._send(400, b"bad destination\n", "text/plain")
            return
        full_dest = unquote(urlparse(raw).path).rstrip("/")
        print(f"  MOVE-DEST {full_dest}", flush=True)
        src_local = self._local(unquote(urlparse(self.path).path))
        dest_local = self._local(full_dest)
        if src_local is None or dest_local is None:
            self._send(409, b"conflict\n", "text/plain")
            return
        src = src_local.rstrip("/")
        dest = dest_local.rstrip("/")
        entry = self._lookup(src) if self.webdav else None
        src_dir, _, _ = src.rpartition("/")
        dst_dir, _, dst_name = dest.rpartition("/")
        siblings = self.tree.get(dst_dir or "/")
        if entry is None or siblings is None or not dst_name:
            self._send(409, b"conflict\n", "text/plain")
            return
        self.tree[src_dir or "/"].remove(entry)
        entry.name = dst_name
        siblings.append(entry)
        self._send(201, b"", "text/plain")

    def do_OPTIONS(self) -> None:
        extra = {"Allow": "OPTIONS, GET, HEAD, PROPFIND"}
        if self.webdav:
            extra["DAV"] = "1"
        self._send(200, b"", "text/plain", extra)

    @staticmethod
    def _response(href: str, displayname: str, entry: Entry | None) -> str:
        """One <D:response>; @p entry None is the queried collection itself."""
        is_dir = entry is None or entry.content is None
        rtype = "<D:collection/>" if is_dir else ""
        length = "" if is_dir or entry is None else (
            f"<D:getcontentlength>{len(entry.content or b'')}</D:getcontentlength>")
        disp = f"<D:displayname>{displayname}</D:displayname>" if displayname else ""
        return (f"<D:response><D:href>{href}</D:href><D:propstat><D:prop>"
                f"<D:resourcetype>{rtype}</D:resourcetype>{disp}{length}"
                f"</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
                f"</D:response>")

    def do_PROPFIND(self) -> None:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length > 0:
            self.rfile.read(length)
        local = self._local(unquote(urlparse(self.path).path))
        entries = self._entries(local) if self.webdav and local is not None else None
        if local is None or entries is None:
            self._send(404, b"not found\n", "text/plain")
            return
        base = (self.base + local).rstrip("/")
        parts = [] if self.no_self else [self._response(quote(base + "/"), "", None)]
        if self.headers.get("Depth", "1") != "0":
            for e in entries:
                href = (e.href.replace("{B}", quote(self.base)) if e.href is not None
                        else quote(f"{base}/{e.name}"))
                disp = e.displayname if e.displayname is not None else escape(e.name)
                parts.append(self._response(href, disp, e))
        body = ('<?xml version="1.0" encoding="utf-8"?>'
                '<D:multistatus xmlns:D="DAV:">' + "".join(parts)
                + "</D:multistatus>")
        self._send(207, body.encode(), 'application/xml; charset="utf-8"')

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002
        print(f"  {self.address_string()} {format % args}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Serve a synthetic tree to the axl-webfs mount driver.")
    parser.add_argument("--case", required=True,
                        choices=["long-names", "entities", "big", "huge", "relaxed",
                                 "based", "deep"])
    parser.add_argument("--base", default="",
                        help="serve the tree under this (decoded) path prefix")
    parser.add_argument("--no-self", action="store_true",
                        help="leave the queried collection out of each PROPFIND")
    parser.add_argument("--port", type=int, default=9877)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--webdav", action="store_true",
                        help="speak WebDAV instead of the JSON protocol")
    args = parser.parse_args()
    ListingHandler.webdav = args.webdav
    ListingHandler.tree = build_tree(args.case)
    ListingHandler.case = args.case
    ListingHandler.base = args.base.rstrip("/")
    ListingHandler.no_self = args.no_self
    server = HTTPServer((args.bind, args.port), ListingHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()


if __name__ == "__main__":
    main()
