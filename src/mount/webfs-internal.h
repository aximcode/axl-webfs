/** @file
  axl-webfs-mount -- internal types and declarations.

  Post-Phase-C migration to <axl/axl-fs-provider.h>: this driver no
  longer synthesizes EFI_FILE_PROTOCOL or
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL vtables. The SDK does that. We
  fill an `AxlFsProvider` callback table speaking pure UTF-8 paths
  and `AxlFsStatus` return codes; the SDK marshals UCS-2 ↔ UTF-8,
  EFI_FILE_INFO header+trailer, and EFI_STATUS mapping at the
  boundary.

  The two opaque types are:
    - `WebFsPrivate`   — per-mount state (HTTP client, cache, base
                          URL). Lives in `AxlFsProvider.backend_ctx`.
    - `AxlFsProviderFile` — per-open-file state. Provider owns the
                            type and its layout; SDK holds it
                            opaque and threads it back into our
                            callbacks.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef WEBFS_INTERNAL_H_
#define WEBFS_INTERNAL_H_

#include <axl.h>
#include <axl/axl-cache.h>
#include <axl/axl-digest.h>
#include <axl/axl-fs-provider.h>
#include <axl/axl-json.h>
#include <axl/axl-net.h>
#include <axl/axl-log.h>
#include <axl/axl-url.h>



// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define DIR_CACHE_TTL_MS             2000     ///< 2 second directory cache TTL.
#define DIR_CACHE_MAX_SLOTS          16       ///< Maximum cached directories.
#define DIR_CACHE_MAX_ENTRIES        64       ///< Max files per cached directory.
#define READAHEAD_BUF_SIZE           (64 * 1024)
#define MAX_PATH_LEN                 512
#define HTTP_BODY_BUF_SIZE           4096
/* axl-webfs's chosen default. Picked to avoid the well-trodden
   8080 / 3000 / 5000 / 9000 dev-tool zone — those collide with
   Jenkins, dev servers, SonarQube, Tomcat, etc., especially on
   corporate laptops. 9876 is IANA-registered to RFC 2974 "sd"
   (Session Director, practically defunct), nowhere near the
   ephemeral-port range, and easy to remember. xfer-server.py's
   default tracks this value so a no-flag mount and a no-flag
   serve agree out of the box. Override with `--port N` on serve
   or with `http://host:N/` on mount. */
#define DEFAULT_SERVER_PORT          9876

/* PUT body staging. Each provider->write call appends to a heap
   buffer instead of issuing an immediate PUT; close / flush drain
   the buffer in one PUT so the destination receives the full
   composed body. Without this, UEFI Shell's `cp` (which writes in
   sub-file chunks via the SDK thunk) produced destinations that
   contained only the last chunk's bytes.
   - INITIAL is a small starting cap; the buffer grows geometrically.
   - MAX bounds peak memory so a runaway write doesn't OOM the
     UEFI image. 256 MB is generous for typical UEFI Shell `cp`
     of diagnostic files. */
#define PUT_BUF_INITIAL              (64 * 1024)
#define PUT_BUF_MAX                  (256u * 1024u * 1024u)

#define VOLUME_LABEL_UTF8            "XferMount"

// ---------------------------------------------------------------------------
// Directory cache types
// ---------------------------------------------------------------------------

/// A single file or directory entry from a JSON listing.
typedef struct {
    char       name[256];
    uint64_t   size;
    bool       is_dir;
    char       modified[32];   ///< ISO 8601 timestamp from server.
} DirEntry;

/// One cached directory listing. Stored in axl-sdk's AxlCache
/// (TTL + LRU eviction); webfs-cache.c just wraps the SDK with the
/// listing-fetch glue.
typedef struct {
    DirEntry   entries[DIR_CACHE_MAX_ENTRIES];
    size_t     entry_count;
} DirCacheSlot;

// ---------------------------------------------------------------------------
// Per-mount state. Lives in AxlFsProvider.backend_ctx; threaded
// into the provider callbacks via the AxlFsProviderFile they create.
// ---------------------------------------------------------------------------

typedef struct WebFsPrivate {
    /// Opaque handle returned by axl_fs_provider_publish. Stored so
    /// teardown can call axl_fs_provider_unpublish.
    void                               *fs_handle;

    /// Server connection.
    AxlHttpClient                      *http_client;
    uint8_t                             server_addr[4];
    uint16_t                            server_port;
    char                                base_path[256];
    char                                base_url[280];
    bool                                read_only;
    /// Wire protocol; resolved at mount_setup (probe + override).
    /// WEBFS_PROTO_JSON is the historical default. Value is the
    /// integer index into the protocol vtable (see webfs-protocol.h).
    int                                 protocol;

    /// Directory listing cache (TTL + LRU).
    AxlCache                           *dir_cache;
} WebFsPrivate;

// ---------------------------------------------------------------------------
// Per-open-file context. Definition of AxlFsProviderFile — the
// SDK declares it as an opaque tag in <axl/axl-fs-provider.h>; the
// provider chooses its layout. SDK never reads inside.
// ---------------------------------------------------------------------------

struct AxlFsProviderFile {
    WebFsPrivate        *priv;

    char                 path[MAX_PATH_LEN];   ///< absolute UTF-8, '/' separators
    bool                 is_dir;
    bool                 is_root;              ///< open("/") returns the root
    uint64_t             file_size;
    uint64_t             position;

    /// Directory iteration state.
    DirEntry            *dir_entries;
    size_t               dir_entry_count;
    size_t               dir_read_index;
    bool                 dir_loaded;

    /// Read-ahead buffer (allocated for files, NULL for dirs).
    uint8_t             *read_ahead_buf;
    uint64_t             read_ahead_start;
    size_t               read_ahead_len;

    /// Buffered PUT body. provider->write appends here so multi-
    /// chunk writes from UEFI Shell `cp` compose into a single PUT
    /// at close / flush time. Without this, each write would
    /// PUT-overwrite the destination with just that chunk.
    /// Lifetime: NULL → not dirty (no Writes happened); allocated
    /// on first Write, freed on flush (close OR flush).
    uint8_t             *put_buf;
    size_t               put_buf_cap;
    size_t               put_buf_used;
    bool                 put_dirty;

    /// Built-in SHA-256 integrity check (best-effort). Set up on
    /// open if the server advertises a `Digest: sha-256=<hex>`
    /// header. digest_ctx is fed bytes as long as reads stay
    /// strictly sequential from offset 0; any seek or out-of-order
    /// read latches digest_failed and disables validation. On
    /// close, if the file was read end-to-end sequentially and the
    /// accumulated hash matches digest_expected, the transfer is
    /// verified; on mismatch the close path returns
    /// AXL_FS_ERR_VOLUME_CORRUPTED.
    AxlChecksum         *digest_ctx;
    char                 digest_expected[65];  // 64 hex + NUL
    uint64_t             digest_consumed;       // bytes fed so far
    bool                 digest_want;           // set on open for files
    bool                 digest_active;
    bool                 digest_failed;
};

// ---------------------------------------------------------------------------
// Forward declarations -- webfs-file.c
// ---------------------------------------------------------------------------

/// Vtable filled by webfs-file.c, passed to axl_fs_provider_publish
/// (with the per-mount priv set as backend_ctx).
extern const AxlFsProvider webfs_provider;

// ---------------------------------------------------------------------------
// Forward declarations -- webfs-cache.c
// ---------------------------------------------------------------------------

int  dir_cache_fetch(WebFsPrivate *priv, const char *path, DirEntry **entries, size_t *count);
int  dir_cache_lookup_entry(WebFsPrivate *priv, const char *dir_path, const char *name, DirEntry *entry);
void dir_cache_invalidate(WebFsPrivate *priv, const char *path);

/// Issue an HTTP request with automatic reconnect on connection error.
/// Caller must free *response with axl_http_client_response_free().
int webfs_http_request(WebFsPrivate *priv, const char *method, const char *path, AxlHashTable *extra_headers, const void *body, size_t body_len, AxlHttpClientResponse **response);

/// Streaming-PUT/POST variant: builds the URL exactly like
/// webfs_http_request, then ships the body in chunks through
/// axl-sdk's axl_http_request_streaming so the client never
/// materializes a second body-sized buffer for the send.
int webfs_http_request_buf_streaming(WebFsPrivate *priv, const char *method, const char *path, AxlHashTable *extra_headers, const void *body, size_t body_len, const char *content_type, AxlHttpClientResponse **response);

/// Extract the SHA-256 hex digest from a parsed `Digest:` header value
/// per RFC 3230 (e.g. `sha-256=<64-hex>` or
/// `md5=<hex>, sha-256=<hex>`). Returns 0 on success and copies
/// 64 lowercase hex chars + NUL into @p out_hex. Returns -1 if the
/// header is missing or carries no sha-256 fragment.
int webfs_digest_parse_sha256(const char *header_value, char *out_hex, size_t hex_size);

#endif // WEBFS_INTERNAL_H_
