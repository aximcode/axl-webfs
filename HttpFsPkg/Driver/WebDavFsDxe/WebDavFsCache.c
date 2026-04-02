/** @file
  WebDavFsDxe -- Directory cache and HTTP request helper (axl-cc port).

  Caches GET /list/ responses for 2 seconds to avoid hammering the
  network on repeated ls/access patterns. Provides auto-reconnect
  HTTP request wrapper.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "WebDavFsInternal.h"

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Cache operations
// ---------------------------------------------------------------------------

/// Find a cache slot by path. Returns NULL if not found or expired.
static DirCacheSlot *
DirCacheFind(
    WEBDAVFS_PRIVATE *Private,
    const char       *Path
)
{
    uint64_t Now = axl_time_get_ms();

    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        DirCacheSlot *Slot = &Private->DirCache[i];
        if (!Slot->Valid) continue;
        if (axl_strcmp(Slot->Path, Path) != 0) continue;

        // Check TTL
        uint64_t Age = Now - Slot->TimestampMs;
        if (Age > DIR_CACHE_TTL_MS) {
            Slot->Valid = false;
            return NULL;
        }
        return Slot;
    }
    return NULL;
}

/// Store a directory listing in the cache (LRU eviction of oldest slot).
static void
DirCachePut(
    WEBDAVFS_PRIVATE *Private,
    const char       *Path,
    DirEntry         *Entries,
    size_t            EntryCount
)
{
    // Find existing slot or oldest/empty slot
    DirCacheSlot *Target = NULL;
    uint64_t OldestTs = UINT64_MAX;

    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        DirCacheSlot *Slot = &Private->DirCache[i];

        if (Slot->Valid && axl_strcmp(Slot->Path, Path) == 0) {
            Target = Slot;
            break;
        }
        if (!Slot->Valid) {
            if (Target == NULL) Target = Slot;
            continue;
        }
        if (Slot->TimestampMs < OldestTs) {
            OldestTs = Slot->TimestampMs;
            Target = Slot;
        }
    }

    if (Target == NULL) Target = &Private->DirCache[0];

    axl_strlcpy(Target->Path, Path, MAX_PATH_LEN);
    if (EntryCount > DIR_CACHE_MAX_ENTRIES) {
        EntryCount = DIR_CACHE_MAX_ENTRIES;
    }
    axl_memcpy(Target->Entries, Entries, EntryCount * sizeof(DirEntry));
    Target->EntryCount = EntryCount;
    Target->TimestampMs = axl_time_get_ms();
    Target->Valid = true;
}

void
DirCacheInvalidate(
    WEBDAVFS_PRIVATE *Private,
    const char       *Path
)
{
    for (size_t i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        if (Private->DirCache[i].Valid &&
            axl_strcmp(Private->DirCache[i].Path, Path) == 0) {
            Private->DirCache[i].Valid = false;
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP request with auto-reconnect
// ---------------------------------------------------------------------------

int
WebDavFsHttpRequest(
    WEBDAVFS_PRIVATE       *Private,
    const char             *Method,
    const char             *Path,
    AxlHashTable           *ExtraHeaders,
    const void             *Body,
    size_t                  BodyLen,
    AxlHttpClientResponse **Response
)
{
    // Build full URL from base + path
    char Url[MAX_PATH_LEN + 280];
    axl_snprintf(Url, sizeof(Url), "%s%s", Private->BaseUrl, Path);

    int Ret = axl_http_request(
        Private->HttpClient, Method, Url, Body, BodyLen,
        NULL, ExtraHeaders, Response);

    if (Ret != 0) {
        // Attempt reconnect: destroy client and recreate
        axl_http_client_free(Private->HttpClient);
        Private->HttpClient = axl_http_client_new();
        if (Private->HttpClient == NULL) return -1;

        // Retry once
        Ret = axl_http_request(
            Private->HttpClient, Method, Url, Body, BodyLen,
            NULL, ExtraHeaders, Response);
    }

    return Ret;
}

// ---------------------------------------------------------------------------
// Fetch directory listing (cache or HTTP)
// ---------------------------------------------------------------------------

int
DirCacheFetch(
    WEBDAVFS_PRIVATE *Private,
    const char       *Path,
    DirEntry        **Entries,
    size_t           *EntryCount
)
{
    // Check cache first
    DirCacheSlot *Slot = DirCacheFind(Private, Path);
    if (Slot != NULL) {
        *Entries = Slot->Entries;
        *EntryCount = Slot->EntryCount;
        return 0;
    }

    // Cache miss -- fetch from server
    char ListPath[MAX_PATH_LEN];
    axl_snprintf(ListPath, sizeof(ListPath), "/list%s", Path);

    AxlHttpClientResponse *Response = NULL;
    int Ret = WebDavFsHttpRequest(
        Private, "GET", ListPath, NULL, NULL, 0, &Response);
    if (Ret != 0 || Response == NULL) return -1;

    if (Response->status_code == 404) {
        axl_http_client_response_free(Response);
        return -1;
    }
    if (Response->status_code != 200) {
        axl_http_client_response_free(Response);
        return -1;
    }

    // NUL-terminate body for JSON parsing
    size_t BodySize = Response->body_size;
    if (BodySize >= HTTP_BODY_BUF_SIZE) BodySize = HTTP_BODY_BUF_SIZE - 1;
    char BodyBuf[HTTP_BODY_BUF_SIZE];
    axl_memcpy(BodyBuf, Response->body, BodySize);
    BodyBuf[BodySize] = '\0';
    axl_http_client_response_free(Response);

    // Parse JSON array
    AxlJsonCtx Ctx;
    if (!axl_json_parse(BodyBuf, BodySize, &Ctx)) return -1;

    // Must be a root-level array
    AxlJsonArrayIter Iter;
    if (!axl_json_root_array_begin(&Ctx, &Iter)) return -1;

    DirEntry TempEntries[DIR_CACHE_MAX_ENTRIES];
    size_t Count = 0;

    AxlJsonCtx Elem;
    while (axl_json_array_next(&Iter, &Elem) &&
           Count < DIR_CACHE_MAX_ENTRIES) {
        DirEntry *E = &TempEntries[Count];
        axl_memset(E, 0, sizeof(*E));

        axl_json_get_string(&Elem, "name", E->Name, sizeof(E->Name));
        axl_json_get_uint(&Elem, "size", &E->Size);
        axl_json_get_bool(&Elem, "dir", &E->IsDir);
        axl_json_get_string(&Elem, "modified", E->Modified, sizeof(E->Modified));

        if (E->Name[0] != '\0') Count++;
    }

    // Store in cache
    DirCachePut(Private, Path, TempEntries, Count);

    // Return from cache (stable pointers)
    Slot = DirCacheFind(Private, Path);
    if (Slot != NULL) {
        *Entries = Slot->Entries;
        *EntryCount = Slot->EntryCount;
        return 0;
    }

    return -1;
}

int
DirCacheLookupEntry(
    WEBDAVFS_PRIVATE *Private,
    const char       *DirPath,
    const char       *Name,
    DirEntry         *Entry
)
{
    DirEntry *Entries = NULL;
    size_t Count = 0;

    int Ret = DirCacheFetch(Private, DirPath, &Entries, &Count);
    if (Ret != 0) return -1;

    for (size_t i = 0; i < Count; i++) {
        if (axl_strcasecmp(Entries[i].Name, Name) == 0) {
            axl_memcpy(Entry, &Entries[i], sizeof(DirEntry));
            return 0;
        }
    }

    return -1;
}
