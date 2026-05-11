/** @file
  axl-webfs-mount -- internal types and declarations.

  Private data structures for the remote filesystem driver.
  Uses SIGNATURE_32 + CR macros for container derivation.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef WEBFS_INTERNAL_H_
#define WEBFS_INTERNAL_H_

#include <axl.h>
#include <axl/axl-cache.h>
#include <axl/axl-digest.h>
#include <axl/axl-json.h>
#include <axl/axl-net.h>
#include <axl/axl-log.h>
#include <axl/axl-url.h>
#include <uefi/axl-uefi.h>



// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define WEBFS_PRIVATE_SIGNATURE   AXL_SIGNATURE_32('W','D','F','S')
#define WEBFS_FILE_SIGNATURE      AXL_SIGNATURE_32('W','D','F','L')

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

/* PUT body staging. Each WebFsWrite call appends to a heap buffer
   instead of issuing an immediate PUT; WebFsClose / WebFsFlush
   drain the buffer in one PUT so the destination receives the full
   composed body. Without this, UEFI Shell's `cp` (which writes in
   sub-file chunks) produced destinations that contained only the
   last chunk's bytes.
   - INITIAL is a small starting cap; the buffer grows geometrically.
   - MAX bounds peak memory so a runaway write doesn't OOM the
     UEFI image. 256 MB is generous for typical UEFI Shell `cp`
     of diagnostic files; multi-GB transfers need future temp-file-
     backed staging or true producer-driven streaming via the SDK's
     axl_http_request_streaming. */
#define PUT_BUF_INITIAL              (64 * 1024)
#define PUT_BUF_MAX                  (256u * 1024u * 1024u)

#define VOLUME_LABEL                 L"XferMount"

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
// Driver-side runtime context (one per mount). Tag (struct WebFsPrivate)
// matches the opaque forward decl in mount/webfs-mount.h so MountOpts.priv
// can reference it without dragging UEFI types into the launcher build.
// ---------------------------------------------------------------------------

typedef struct WebFsPrivate {
    UINT32                              signature;
    void                               *fs_handle;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     simple_fs;
    EFI_DEVICE_PATH_PROTOCOL            *device_path;

    /// Server connection.
    AxlHttpClient                       *http_client;
    uint8_t                             server_addr[4];
    uint16_t                            server_port;
    char                                base_path[256];
    char                                base_url[280];
    bool                                read_only;
    /// Wire protocol; resolved at mount_setup (probe + override).
    /// WEBFS_PROTO_JSON is the historical default. Value is the
    /// integer index into the protocol vtable (see webfs-protocol.h).
    int                                 protocol;

    /// Directory listing cache (TTL + LRU). axl-sdk's AxlCache
    /// owns slot allocation, eviction, and TTL checks; webfs-cache.c
    /// just builds DirCacheSlot values and looks them up by path.
    AxlCache                           *dir_cache;
} WebFsPrivate;

#define WEBFS_PRIVATE_FROM_SIMPLE_FS(a) \
    AXL_CONTAINER_OF(a, WebFsPrivate, simple_fs)

// ---------------------------------------------------------------------------
// Per-file-handle context (one per Open)
// ---------------------------------------------------------------------------

typedef struct {
    UINT32               signature;
    EFI_FILE_PROTOCOL    file;
    WebFsPrivate      *private_data;

    char                 path[MAX_PATH_LEN];
    bool                 is_dir;
    bool                 is_root;
    uint64_t             file_size;
    uint64_t             position;

    /// Directory iteration state.
    DirEntry             *dir_entries;
    size_t               dir_entry_count;
    size_t               dir_read_index;
    bool                 dir_loaded;

    /// Read-ahead buffer (allocated for files, NULL for dirs).
    uint8_t              *read_ahead_buf;
    uint64_t             read_ahead_start;
    size_t               read_ahead_len;


    /// Buffered PUT body. WebFsWrite appends here so multi-chunk
    /// Writes from UEFI Shell `cp` compose into a single PUT at
    /// WebFsClose / WebFsFlush time. Without this, each Write
    /// would PUT-overwrite the destination with just that chunk.
    /// Lifetime: NULL → not dirty (no Writes happened); allocated
    /// on first Write, freed on flush (Close OR Flush). Position
    /// always matches put_buf_used (sequential append model);
    /// SetPosition / non-sequential Writes are flagged but the
    /// model breaks gracefully (last buffered window wins).
    uint8_t              *put_buf;
    size_t               put_buf_cap;
    size_t               put_buf_used;
    bool                 put_dirty;

    /// Built-in SHA-256 integrity check (best-effort). Set up on
    /// Open if the server advertises a `Digest: sha-256=<hex>` header.
    /// digest_ctx is fed bytes as long as reads stay strictly
    /// sequential from offset 0; any seek or out-of-order Read
    /// latches digest_failed and disables validation. On Close, if
    /// the file was read end-to-end sequentially and the accumulated
    /// hash matches digest_expected, the transfer is verified; on
    /// mismatch the close path returns EFI_VOLUME_CORRUPTED.
    AxlChecksum          *digest_ctx;
    char                 digest_expected[65];  // 64 hex + NUL
    uint64_t             digest_consumed;       // bytes fed so far
    bool                 digest_want;           // set on Open for files
    bool                 digest_active;
    bool                 digest_failed;
} WebFsFileCtx;

#define WEBFS_FILE_FROM_FILE_PROTOCOL(a) \
    AXL_CONTAINER_OF(a, WebFsFileCtx, file)

// ---------------------------------------------------------------------------
// Forward declarations -- webfs-file.c
// ---------------------------------------------------------------------------

EFI_STATUS EFIAPI WebFsOpen(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
EFI_STATUS EFIAPI WebFsClose(EFI_FILE_PROTOCOL *This);
EFI_STATUS EFIAPI WebFsDelete(EFI_FILE_PROTOCOL *This);
EFI_STATUS EFIAPI WebFsRead(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebFsWrite(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebFsGetPosition(EFI_FILE_PROTOCOL *This, UINT64 *Position);
EFI_STATUS EFIAPI WebFsSetPosition(EFI_FILE_PROTOCOL *This, UINT64 Position);
EFI_STATUS EFIAPI WebFsGetInfo(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebFsSetInfo(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebFsFlush(EFI_FILE_PROTOCOL *This);

/// Allocate and initialize a WebFsFileCtx with all function pointers wired.
WebFsFileCtx * webfs_create_file_handle(WebFsPrivate *priv, const char *path, bool is_dir, uint64_t file_size);

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
/// axl-sdk's axl_http_request_streaming (SDK 14cef93) so the
/// client never materializes a second body-sized buffer for the
/// send. Body comes from @p body / @p body_len — the helper wraps
/// it with a tiny producer-callback that drains the buffer at
/// whatever chunk size the SDK pulls. For consumers that have a
/// real producer (future temp-file-backed staging), use
/// axl_http_request_streaming directly with a custom callback.
int webfs_http_request_buf_streaming(WebFsPrivate *priv, const char *method, const char *path, AxlHashTable *extra_headers, const void *body, size_t body_len, const char *content_type, AxlHttpClientResponse **response);

/// Extract the SHA-256 hex digest from a parsed `Digest:` header value
/// per RFC 3230 (e.g. `sha-256=<64-hex>` or
/// `md5=<hex>, sha-256=<hex>`). Returns 0 on success and copies
/// 64 lowercase hex chars + NUL into @p out_hex. Returns -1 if the
/// header is missing or carries no sha-256 fragment.
int webfs_digest_parse_sha256(const char *header_value, char *out_hex, size_t hex_size);

#endif // WEBFS_INTERNAL_H_
