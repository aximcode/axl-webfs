/** @file
  axl-webfs-mount -- protocol abstraction.

  The mount driver speaks one of two wire protocols to the
  workstation server:

    JSON   bespoke HTTP/JSON (xfer-server.py default mode):
           GET /list/<path>  GET /files/<path>  PUT /files/<path>
           POST /files/<path>?mkdir   POST /files/<path>?rename=<n>
           DELETE /files/<path>

    DAV    RFC 4918 WebDAV class-1 + MOVE (any WebDAV server,
           incl. wsgidav-backed xfer-server.py, Apache mod_dav,
           NextCloud):
           PROPFIND /<path> Depth:1   GET/PUT /<path>
           MKCOL /<path>   MOVE + Destination:   DELETE /<path>

  Each protocol implements the same callback table; the driver
  picks one at mount time (OPTIONS probe → DAV: header → DAV,
  otherwise JSON; can be overridden via the mount --protocol flag).

  All ops are blocking and synchronous — the EFI_FILE_PROTOCOL
  caller (Shell, LoadImage, etc.) drives the tick. Return 0 on
  success, -1 on error. HTTP status codes are surfaced via the
  out-params where the caller needs to distinguish 4xx semantics
  from network failure.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef WEBFS_PROTOCOL_H_
#define WEBFS_PROTOCOL_H_

#include "webfs-internal.h"

typedef enum {
    WEBFS_PROTO_JSON = 0,
    WEBFS_PROTO_DAV  = 1,
} WebfsProtocol;

/// Per-protocol operation table. JSON impl extracted from the
/// pre-WebDAV mount driver; DAV impl added alongside.
typedef struct {
    /// Probe the server with the protocol's "ping" verb. Returns 0
    /// if the server speaks this protocol (JSON: GET /info success;
    /// DAV: OPTIONS / responds with DAV: header). Mount uses this
    /// to choose at runtime.
    int (*probe)(WebFsPrivate *priv);

    /// Fetch a directory listing. Caller passes a buffer of
    /// DirEntry; fills @p out_count.
    int (*list_dir)(WebFsPrivate *priv, const char *path,
                    DirEntry *out, size_t max, size_t *out_count);

    /// Fetch a byte range. Caller's @p buf has room for @p len bytes;
    /// @p out_got is set to the number actually returned (≤ len).
    /// @p out_digest_hex (optional, NULL to skip) receives 64
    /// lowercase hex chars + NUL of the file's `Digest: sha-256=...`
    /// response header, when present. Servers that don't advertise
    /// it leave the buffer untouched and the call still succeeds.
    int (*read_range)(WebFsPrivate *priv, const char *path,
                      uint64_t offset, size_t len,
                      void *buf, size_t *out_got,
                      char *out_digest_hex, size_t digest_size);

    /// Upload (or overwrite) a file with @p len bytes of @p body.
    /// On success @p out_status is set to the server's status code
    /// (201 created, 200 overwrite, etc.) for caller diagnostics.
    int (*write_full)(WebFsPrivate *priv, const char *path,
                      const void *body, size_t len,
                      size_t *out_status);

    /// Create an empty file (PUT with zero-length body). Separate
    /// from write_full because some servers care about the
    /// distinction (and the JSON path uses an empty string vs NULL).
    int (*create_empty)(WebFsPrivate *priv, const char *path,
                        size_t *out_status);

    /// Create a directory.
    int (*mkdir)(WebFsPrivate *priv, const char *path,
                 size_t *out_status);

    /// Remove a file or empty directory.
    int (*remove)(WebFsPrivate *priv, const char *path,
                  size_t *out_status);

    /// Rename a file or directory. @p new_name is a basename within
    /// the same parent directory (matches the existing JSON
    /// protocol's idiom); cross-directory rename happens at the
    /// caller layer by chaining mkdir + remove if needed.
    int (*rename)(WebFsPrivate *priv, const char *old_path,
                  const char *new_name, size_t *out_status);
} WebfsProtocolOps;

extern const WebfsProtocolOps webfs_proto_json;
extern const WebfsProtocolOps webfs_proto_dav;

static inline const WebfsProtocolOps *
webfs_protocol_ops(WebfsProtocol p)
{
    return (p == WEBFS_PROTO_DAV) ? &webfs_proto_dav : &webfs_proto_json;
}

#endif /* WEBFS_PROTOCOL_H_ */
