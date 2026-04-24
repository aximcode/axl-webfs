/** @file
  axl-webfs-dxe -- Directory cache and HTTP request helper.

  Caches GET /list/ responses for 2 seconds to avoid hammering the
  network on repeated ls/access patterns. Provides auto-reconnect
  HTTP request wrapper.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-internal.h"

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Cache operations
// ---------------------------------------------------------------------------

/// Find a cache slot by path. Returns NULL if not found or expired.
static DirCacheSlot *
dir_cache_find(
    WebFsPrivate *priv,
    const char      *path
)
{
    uint64_t now = axl_time_get_ms();

    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        DirCacheSlot *slot = &priv->dir_cache[i];
        if (!slot->valid) continue;
        if (!axl_streql(slot->path, path)) continue;

        // Check TTL
        uint64_t age = now - slot->timestamp_ms;
        if (age > DIR_CACHE_TTL_MS) {
            slot->valid = false;
            return NULL;
        }
        return slot;
    }
    return NULL;
}

/// Store a directory listing in the cache (LRU eviction of oldest slot).
static void
dir_cache_put(
    WebFsPrivate *priv,
    const char      *path,
    DirEntry        *entries,
    size_t           count
)
{
    // Find existing slot or oldest/empty slot
    DirCacheSlot *target = NULL;
    uint64_t oldest_ts = UINT64_MAX;

    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        DirCacheSlot *slot = &priv->dir_cache[i];

        if (slot->valid && axl_streql(slot->path, path)) {
            target = slot;
            break;
        }
        if (!slot->valid) {
            if (target == NULL) target = slot;
            continue;
        }
        if (slot->timestamp_ms < oldest_ts) {
            oldest_ts = slot->timestamp_ms;
            target = slot;
        }
    }

    if (target == NULL) target = &priv->dir_cache[0];

    axl_strlcpy(target->path, path, MAX_PATH_LEN);
    if (count > DIR_CACHE_MAX_ENTRIES) {
        count = DIR_CACHE_MAX_ENTRIES;
    }
    axl_memcpy(target->entries, entries, count * sizeof(DirEntry));
    target->entry_count = count;
    target->timestamp_ms = axl_time_get_ms();
    target->valid = true;
}

void
dir_cache_invalidate(
    WebFsPrivate *priv,
    const char      *path
)
{
    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        if (priv->dir_cache[i].valid &&
            axl_streql(priv->dir_cache[i].path, path)) {
            priv->dir_cache[i].valid = false;
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP request with auto-reconnect
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Fetch directory listing (cache or HTTP)
// ---------------------------------------------------------------------------

int
dir_cache_fetch(
    WebFsPrivate *priv,
    const char      *path,
    DirEntry       **entries,
    size_t          *count
)
{
    // Check cache first
    DirCacheSlot *slot = dir_cache_find(priv, path);
    if (slot != NULL) {
        *entries = slot->entries;
        *count = slot->entry_count;
        return 0;
    }

    // Cache miss -- fetch from server
    char list_path[MAX_PATH_LEN];
    axl_snprintf(list_path, sizeof(list_path), "/list%s", path);

    AxlHttpClientResponse *response = NULL;
    int ret = webfs_http_request(
        priv, "GET", list_path, NULL, NULL, 0, &response);
    if (ret != 0 || response == NULL) return -1;

    if (response->status_code == 404) {
        axl_http_client_response_free(response);
        return -1;
    }
    if (response->status_code != 200) {
        axl_http_client_response_free(response);
        return -1;
    }

    // NUL-terminate body for JSON parsing
    size_t body_size = response->body_size;
    if (body_size >= HTTP_BODY_BUF_SIZE) body_size = HTTP_BODY_BUF_SIZE - 1;
    char *body_buf = axl_malloc(body_size + 1);
    if (body_buf == NULL) {
        axl_http_client_response_free(response);
        return -1;
    }
    axl_memcpy(body_buf, response->body, body_size);
    body_buf[body_size] = '\0';
    axl_http_client_response_free(response);

    // Parse JSON array
    AxlJsonCtx ctx;
    if (!axl_json_parse(body_buf, body_size, &ctx)) {
        axl_free(body_buf);
        return -1;
    }

    AxlJsonArrayIter iter;
    if (!axl_json_root_array_begin(&ctx, &iter)) {
        axl_json_free(&ctx);
        axl_free(body_buf);
        return -1;
    }

    DirEntry *temp_entries = axl_calloc(DIR_CACHE_MAX_ENTRIES, sizeof(DirEntry));
    if (temp_entries == NULL) {
        axl_json_free(&ctx);
        axl_free(body_buf);
        return -1;
    }
    size_t n = 0;

    AxlJsonCtx elem;
    while (axl_json_array_next(&iter, &elem) &&
           n < DIR_CACHE_MAX_ENTRIES) {
        DirEntry *e = &temp_entries[n];

        axl_json_get_string(&elem, "name", e->name, sizeof(e->name));
        axl_json_get_uint(&elem, "size", &e->size);
        axl_json_get_bool(&elem, "dir", &e->is_dir);
        axl_json_get_string(&elem, "modified", e->modified, sizeof(e->modified));

        if (e->name[0] != '\0') n++;
    }

    axl_json_free(&ctx);
    axl_free(body_buf);

    // Store in cache
    dir_cache_put(priv, path, temp_entries, n);
    axl_free(temp_entries);

    // Return from cache (stable pointers)
    slot = dir_cache_find(priv, path);
    if (slot != NULL) {
        *entries = slot->entries;
        *count = slot->entry_count;
        return 0;
    }

    return -1;
}

int
dir_cache_lookup_entry(
    WebFsPrivate *priv,
    const char      *dir_path,
    const char      *name,
    DirEntry        *entry
)
{
    DirEntry *entries = NULL;
    size_t count = 0;

    int ret = dir_cache_fetch(priv, dir_path, &entries, &count);
    if (ret != 0) return -1;

    for (size_t i = 0; i < count; i++) {
        if (axl_strcasecmp(entries[i].name, name) == 0) {
            axl_memcpy(entry, &entries[i], sizeof(DirEntry));
            return 0;
        }
    }

    return -1;
}
