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

// ----------------------------------------------------------------------------
// HTTP request with auto-reconnect
// ----------------------------------------------------------------------------

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
    // URL-encode the path portion (preserves /), keep query string raw
    char encoded_path[MAX_PATH_LEN * 3];
    const char *query = path;
    while (*query && *query != '?') query++;
    if (*query == '\0') query = NULL;
    if (query != NULL) {
        char path_only[MAX_PATH_LEN];
        size_t plen = (size_t)(query - path);
        if (plen >= sizeof(path_only)) plen = sizeof(path_only) - 1;
        axl_memcpy(path_only, path, plen);
        path_only[plen] = '\0';
        char enc_part[MAX_PATH_LEN * 3];
        axl_url_encode(path_only, enc_part, sizeof(enc_part));
        axl_snprintf(encoded_path, sizeof(encoded_path), "%s%s", enc_part, query);
    } else {
        axl_url_encode(path, encoded_path, sizeof(encoded_path));
    }
    char url[MAX_PATH_LEN * 3 + 280];
    axl_snprintf(url, sizeof(url), "%s%s", priv->base_url, encoded_path);

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
    /* URL-encode path identically to webfs_http_request — same
       split/encode-path / keep-query-raw rule so consumers can
       pass /files/<path>?something safely. */
    char encoded_path[MAX_PATH_LEN * 3];
    const char *query = path;
    while (*query && *query != '?') query++;
    if (*query == '\0') query = NULL;
    if (query != NULL) {
        char path_only[MAX_PATH_LEN];
        size_t plen = (size_t)(query - path);
        if (plen >= sizeof(path_only)) plen = sizeof(path_only) - 1;
        axl_memcpy(path_only, path, plen);
        path_only[plen] = '\0';
        char enc_part[MAX_PATH_LEN * 3];
        axl_url_encode(path_only, enc_part, sizeof(enc_part));
        axl_snprintf(encoded_path, sizeof(encoded_path), "%s%s",
                     enc_part, query);
    } else {
        axl_url_encode(path, encoded_path, sizeof(encoded_path));
    }
    char url[MAX_PATH_LEN * 3 + 280];
    axl_snprintf(url, sizeof(url), "%s%s", priv->base_url, encoded_path);

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

/* `entries` borrow window: callers must finish iterating the
   returned pointer before issuing another dir_cache_fetch on this
   WebFsPrivate. The pointer aliases into this file-static slot,
   which is overwritten on every call (hit OR miss). Today's callers
   (read_dir copies immediately into its own DirEntry array;
   lookup_entry iterates and returns by value within one function)
   honor that. Safe under the single-threaded UEFI model — the
   dispatcher is single-armed, only one EFI_FILE_PROTOCOL call runs
   at a time, so reentrant overwrite isn't a concern; the borrow
   window is purely sequential. */
static DirCacheSlot mLastSlot;

int
dir_cache_fetch(
    WebFsPrivate *priv,
    const char   *path,
    DirEntry    **entries,
    size_t       *count
)
{
    if (priv->dir_cache != NULL &&
        axl_cache_get(priv->dir_cache, path, &mLastSlot) == AXL_OK) {
        *entries = mLastSlot.entries;
        *count   = mLastSlot.entry_count;
        return 0;
    }

    /* Miss — fetch via the active protocol's list_dir. */
    DirCacheSlot fresh;
    axl_memset(&fresh, 0, sizeof(fresh));
    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    if (ops->list_dir(priv, path, fresh.entries,
                      DIR_CACHE_MAX_ENTRIES, &fresh.entry_count) != 0)
        return -1;

    if (priv->dir_cache != NULL) {
        axl_cache_put(priv->dir_cache, path, &fresh);
    }
    mLastSlot = fresh;
    *entries = mLastSlot.entries;
    *count   = mLastSlot.entry_count;
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
    if (priv->dir_cache != NULL)
        axl_cache_invalidate(priv->dir_cache, path);
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
