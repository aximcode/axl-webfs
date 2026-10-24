/** @file
  axl-webfs-mount -- directory cache + HTTP helper.

  Wraps axl-sdk's AxlCache (TTL + LRU) to memoize directory listings
  per path, plus the auto-reconnect HTTP request helper used by both
  protocol implementations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-internal.h"
#include "webfs-protocol.h"

AXL_LOG_DOMAIN("webfs-mount-http");

// ----------------------------------------------------------------------------
// HTTP request with auto-reconnect
// ----------------------------------------------------------------------------

/* Room for base_url + a percent-encoded wire path (3x) + a raw query
   (a rename's encoded new name, 3x WEBFS_NAME_SIZE). */
/* axl_http_request writes the request target (path + "?" + query) into
   512 bytes and silently CUTS a longer one (axl-http-client.c,
   full_path[512]), so a longer target is refused here instead. */
#define WEBFS_REQUEST_TARGET_MAX  512
#define WEBFS_URL_MAX \
    (280 + WEBFS_WIRE_PATH_MAX * 3 + WEBFS_NAME_SIZE * 3 + 16)

/* Build the request URL for @p path: the path portion URL-encoded
   ('/' kept), a "?query" kept raw. false -- logged, never cut -- when a
   part does not fit; a cut URL names another resource. */
static bool
webfs_request_url(WebFsPrivate *priv, const char *path, char *url,
                  size_t size)
{
    const char *query = path;
    while (*query && *query != '?') query++;
    size_t plen = (size_t)(query - path);

    char path_only[WEBFS_WIRE_PATH_MAX];
    char enc[WEBFS_WIRE_PATH_MAX * 3];
    int  n = -1;
    if (plen < sizeof(path_only)) {
        axl_memcpy(path_only, path, plen);
        path_only[plen] = '\0';
        if (axl_url_encode(path_only, enc, sizeof(enc)) >= 0 &&
            axl_strlen(enc) + axl_strlen(query) < WEBFS_REQUEST_TARGET_MAX)
            n = axl_snprintf(url, size, "%s%s%s", priv->base_url, enc, query);
    }
    if (n < 0 || (size_t)n >= size) {
        axl_error("request path is too long (%zu bytes); refused",
                  axl_strlen(path));
        return false;
    }
    return true;
}

int
webfs_http_request(
    WebFsPrivate        *priv,
    const char             *method,
    const char             *path,
    AxlHashTable           *extra_headers,
    const void             *body,
    size_t                  body_len,
    AxlHttpClientResponse **response
)
{
    char url[WEBFS_URL_MAX];
    if (!webfs_request_url(priv, path, url, sizeof(url)))
        return -1;

    return axl_http_request(
        priv->http_client, method, url, body, body_len,
        NULL, extra_headers, response);
}

// ----------------------------------------------------------------------------
// Streaming-PUT helper (buffer-drain producer over axl_http_request_streaming)
// ----------------------------------------------------------------------------

typedef struct {
    const uint8_t *bytes;
    size_t         total;
    size_t         pos;
} PutDrainCtx;

static int
put_drain(void *ctx, void *out_buf, size_t out_size, size_t *out_count)
{
    PutDrainCtx *d = ctx;
    size_t remaining = d->total - d->pos;
    size_t n = remaining < out_size ? remaining : out_size;
    if (n > 0) {
        axl_memcpy(out_buf, d->bytes + d->pos, n);
        d->pos += n;
    }
    *out_count = n;
    return AXL_OK;
}

int
webfs_http_request_buf_streaming(
    WebFsPrivate          *priv,
    const char            *method,
    const char            *path,
    AxlHashTable          *extra_headers,
    const void            *body,
    size_t                 body_len,
    const char            *content_type,
    AxlHttpClientResponse **response
)
{
    char url[WEBFS_URL_MAX];
    if (!webfs_request_url(priv, path, url, sizeof(url)))
        return -1;

    PutDrainCtx d = { .bytes = body, .total = body_len, .pos = 0 };
    return axl_http_request_streaming(
        priv->http_client, method, url,
        put_drain, &d, NULL,
        body_len, content_type,
        extra_headers, response);
}

// ----------------------------------------------------------------------------
// Directory listing cache
// ----------------------------------------------------------------------------

/* A cached listing. path NULL = a free slot. */
typedef struct {
    char     *path;
    AxlArray *entries;      ///< of DirEntry
    uint64_t  fetched_ms;   ///< for the TTL
    uint64_t  used;         ///< LRU tick
} DirCacheSlot;

struct DirCache {
    DirCacheSlot slots[DIR_CACHE_MAX_SLOTS];
    uint64_t     tick;
};

DirCache *
dir_cache_new(void)
{
    return axl_calloc(1, sizeof(DirCache));
}

static void
dir_cache_slot_clear(DirCacheSlot *slot)
{
    axl_free(slot->path);
    axl_array_free(slot->entries);
    axl_memset(slot, 0, sizeof(*slot));
}

void
dir_cache_free(DirCache *cache)
{
    if (cache == NULL) return;
    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++)
        dir_cache_slot_clear(&cache->slots[i]);
    axl_free(cache);
}

int
webfs_listing_append(AxlArray *out, const DirEntry *e, const char *path)
{
    if (axl_array_len(out) >= WEBFS_LIST_MAX_ENTRIES) {
        axl_error("listing %s has more than %u entries; refused",
                  path, (unsigned)WEBFS_LIST_MAX_ENTRIES);
        return -1;
    }
    return axl_array_append(out, e) == AXL_OK ? 0 : -1;
}

/* The live slot for @p path, or NULL. EVERY expired slot is cleared on
   the way -- a big listing nobody asks for again must not stay
   resident until it happens to be evicted. */
static DirCacheSlot *
dir_cache_find(DirCache *cache, const char *path)
{
    uint64_t      now   = axl_time_get_ms();
    DirCacheSlot *found = NULL;
    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        DirCacheSlot *slot = &cache->slots[i];
        if (slot->path == NULL)
            continue;
        if (now - slot->fetched_ms > DIR_CACHE_TTL_MS)
            dir_cache_slot_clear(slot);
        else if (axl_streql(slot->path, path))
            found = slot;
    }
    return found;
}

/* `entries` borrow window: callers must finish iterating the returned
   pointer before the next dir_cache_fetch or dir_cache_invalidate on
   this WebFsPrivate -- either may evict or clear the slot it points
   into. Today's callers (read_dir copies immediately into its own
   DirEntry array; lookup_entry iterates and returns by value within
   one function) honor that, and UEFI's single-armed dispatcher runs one
   EFI_FILE_PROTOCOL call at a time. */
int
dir_cache_fetch(
    WebFsPrivate *priv,
    const char   *path,
    DirEntry    **entries,
    size_t       *count
)
{
    DirCache     *cache = priv->dir_cache;
    DirCacheSlot *slot  = dir_cache_find(cache, path);

    if (slot == NULL) {
        /* Miss -- fetch the whole listing via the active protocol. */
        AxlArray *fresh = axl_array_new(sizeof(DirEntry));
        char     *key   = axl_strdup(path);
        const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
        if (fresh == NULL || key == NULL ||
            ops->list_dir(priv, path, fresh) != 0) {
            axl_array_free(fresh);
            axl_free(key);
            return -1;
        }
        /* A free slot, else the least recently used. */
        slot = &cache->slots[0];
        for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
            DirCacheSlot *s = &cache->slots[i];
            if (s->path == NULL) { slot = s; break; }
            if (s->used < slot->used) slot = s;
        }
        dir_cache_slot_clear(slot);
        slot->path       = key;
        slot->entries    = fresh;
        slot->fetched_ms = axl_time_get_ms();
    }
    slot->used = ++cache->tick;
    *entries = axl_array_data(slot->entries);
    *count   = axl_array_len(slot->entries);
    return 0;
}

int
dir_cache_lookup_entry(
    WebFsPrivate *priv,
    const char   *dir_path,
    const char   *name,
    DirEntry     *entry
)
{
    DirEntry *entries = NULL;
    size_t    count = 0;

    if (dir_cache_fetch(priv, dir_path, &entries, &count) != 0) return -1;

    for (size_t i = 0; i < count; i++) {
        if (axl_strcasecmp(entries[i].name, name) == 0) {
            axl_memcpy(entry, &entries[i], sizeof(DirEntry));
            return 0;
        }
    }
    return -1;
}

void
dir_cache_invalidate(
    WebFsPrivate *priv,
    const char   *path
)
{
    DirCacheSlot *slot = dir_cache_find(priv->dir_cache, path);
    if (slot != NULL)
        dir_cache_slot_clear(slot);
}

// ----------------------------------------------------------------------------
// Digest header parser
// ----------------------------------------------------------------------------

int
webfs_digest_parse_sha256(
    const char *header_value,
    char       *out_hex,
    size_t      hex_size
)
{
    if (header_value == NULL || out_hex == NULL || hex_size < 65)
        return -1;

    /* Header value follows the RFC 3230 grammar: comma-separated
       <algorithm>=<value> pairs, optional whitespace, case-insensitive
       algorithm name. Example values:
         "sha-256=abc123..."
         "MD5=deadbeef, SHA-256=cafe..."
         "sha-256=cafe;q=1, md5=..."  (we ignore parameters)
       We accept either `sha-256=` or `sha256=` since some servers
       drop the dash. */
    const char *p = header_value;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '\0') break;

        const char *alg_start = p;
        while (*p != '\0' && *p != '=' && *p != ',') p++;
        size_t alg_len = (size_t)(p - alg_start);

        bool is_sha256 = (alg_len == 7 &&
                          axl_strncasecmp(alg_start, "sha-256", 7) == 0)
                      || (alg_len == 6 &&
                          axl_strncasecmp(alg_start, "sha256", 6) == 0);

        if (*p != '=') {
            /* malformed entry — skip to next comma */
            while (*p != '\0' && *p != ',') p++;
            continue;
        }
        p++;  /* past '=' */

        const char *val_start = p;
        while (*p != '\0' && *p != ',') p++;
        size_t val_len = (size_t)(p - val_start);

        if (is_sha256 && val_len == 64) {
            for (size_t i = 0; i < 64; i++) {
                char c = val_start[i];
                if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
                out_hex[i] = c;
            }
            out_hex[64] = '\0';
            return 0;
        }
    }

    return -1;
}
