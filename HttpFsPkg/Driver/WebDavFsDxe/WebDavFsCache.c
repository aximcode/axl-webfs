/** @file
  WebDavFsDxe — Directory cache and HTTP request helper.

  Caches GET /list/ responses for 2 seconds to avoid hammering the
  network on repeated ls/access patterns. Provides auto-reconnect
  HTTP request wrapper.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "WebDavFsInternal.h"

// ----------------------------------------------------------------------------
// Timestamp helper
// ----------------------------------------------------------------------------

/// Get current time as milliseconds since midnight (rough, for TTL comparison).
static UINT64 GetCurrentTimestampMs(VOID) {
    EFI_TIME Time;
    EFI_STATUS Status = gRT->GetTime(&Time, NULL);
    if (EFI_ERROR(Status)) return 0;

    return ((UINT64)Time.Hour * 3600 + (UINT64)Time.Minute * 60 +
            (UINT64)Time.Second) * 1000 + (UINT64)(Time.Nanosecond / 1000000);
}

// ----------------------------------------------------------------------------
// Cache operations
// ----------------------------------------------------------------------------

/// Find a cache slot by path. Returns NULL if not found or expired.
static DIR_CACHE_SLOT * DirCacheFind(
    IN WEBDAVFS_PRIVATE  *Private,
    IN CONST CHAR8       *Path
) {
    UINT64 Now = GetCurrentTimestampMs();

    for (UINTN i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        DIR_CACHE_SLOT *Slot = &Private->DirCache[i];
        if (!Slot->Valid) continue;
        if (AsciiStrCmp(Slot->Path, Path) != 0) continue;

        // Check TTL
        UINT64 Age = Now - Slot->TimestampMs;
        if (Age > DIR_CACHE_TTL_MS) {
            Slot->Valid = FALSE;
            return NULL;
        }
        return Slot;
    }
    return NULL;
}

/// Store a directory listing in the cache (LRU eviction of oldest slot).
static VOID DirCachePut(
    IN WEBDAVFS_PRIVATE  *Private,
    IN CONST CHAR8       *Path,
    IN DIR_ENTRY         *Entries,
    IN UINTN             EntryCount
) {
    // Find existing slot or oldest/empty slot
    DIR_CACHE_SLOT *Target = NULL;
    UINT64 OldestTs = MAX_UINT64;

    for (UINTN i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        DIR_CACHE_SLOT *Slot = &Private->DirCache[i];

        if (Slot->Valid && AsciiStrCmp(Slot->Path, Path) == 0) {
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

    AsciiStrCpyS(Target->Path, MAX_PATH_LEN, Path);
    if (EntryCount > DIR_CACHE_MAX_ENTRIES) {
        EntryCount = DIR_CACHE_MAX_ENTRIES;
    }
    CopyMem(Target->Entries, Entries, EntryCount * sizeof(DIR_ENTRY));
    Target->EntryCount = EntryCount;
    Target->TimestampMs = GetCurrentTimestampMs();
    Target->Valid = TRUE;
}

VOID DirCacheInvalidate(
    IN WEBDAVFS_PRIVATE  *Private,
    IN CONST CHAR8       *Path
) {
    for (UINTN i = 0; i < DIR_CACHE_MAX_SLOTS; i++) {
        if (Private->DirCache[i].Valid &&
            AsciiStrCmp(Private->DirCache[i].Path, Path) == 0) {
            Private->DirCache[i].Valid = FALSE;
        }
    }
}

// ----------------------------------------------------------------------------
// HTTP request with auto-reconnect
// ----------------------------------------------------------------------------

EFI_STATUS WebDavFsHttpRequest(
    IN  WEBDAVFS_PRIVATE          *Private,
    IN  CONST CHAR8               *Method,
    IN  CONST CHAR8               *Path,
    IN  AXL_HASH_TABLE            *ExtraHeaders  OPTIONAL,
    IN  CONST VOID                *Body          OPTIONAL,
    IN  UINTN                     BodyLen,
    OUT AXL_HTTP_CLIENT_RESPONSE  **Response
) {
    // Build full URL from base + path
    CHAR8 Url[MAX_PATH_LEN + 280];
    AsciiSPrint(Url, sizeof(Url), "%a%a", Private->BaseUrl, Path);

    EFI_STATUS Status = AxlHttpRequest(
        Private->HttpClient, Method, Url, Body, BodyLen,
        NULL, ExtraHeaders, Response);

    if (EFI_ERROR(Status)) {
        // Attempt reconnect: destroy client and recreate
        AxlHttpClientFree(Private->HttpClient);
        Private->HttpClient = AxlHttpClientNew();
        if (Private->HttpClient == NULL) return EFI_OUT_OF_RESOURCES;

        // Retry once
        Status = AxlHttpRequest(
            Private->HttpClient, Method, Url, Body, BodyLen,
            NULL, ExtraHeaders, Response);
    }

    return Status;
}

// ----------------------------------------------------------------------------
// Fetch directory listing (cache or HTTP)
// ----------------------------------------------------------------------------

EFI_STATUS DirCacheFetch(
    IN  WEBDAVFS_PRIVATE  *Private,
    IN  CONST CHAR8       *Path,
    OUT DIR_ENTRY         **Entries,
    OUT UINTN             *EntryCount
) {
    // Check cache first
    DIR_CACHE_SLOT *Slot = DirCacheFind(Private, Path);
    if (Slot != NULL) {
        *Entries = Slot->Entries;
        *EntryCount = Slot->EntryCount;
        return EFI_SUCCESS;
    }

    // Cache miss — fetch from server
    CHAR8 ListPath[MAX_PATH_LEN];
    AsciiSPrint(ListPath, sizeof(ListPath), "/list%a", Path);

    AXL_HTTP_CLIENT_RESPONSE *Response = NULL;
    EFI_STATUS Status = WebDavFsHttpRequest(
        Private, "GET", ListPath, NULL, NULL, 0, &Response);
    if (EFI_ERROR(Status) || Response == NULL) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;

    if (Response->StatusCode == 404) {
        AxlHttpClientResponseFree(Response);
        return EFI_NOT_FOUND;
    }
    if (Response->StatusCode != 200) {
        AxlHttpClientResponseFree(Response);
        return EFI_DEVICE_ERROR;
    }

    // NUL-terminate body for JSON parsing
    UINTN BodySize = Response->BodySize;
    if (BodySize >= HTTP_BODY_BUF_SIZE) BodySize = HTTP_BODY_BUF_SIZE - 1;
    CHAR8 BodyBuf[HTTP_BODY_BUF_SIZE];
    CopyMem(BodyBuf, Response->Body, BodySize);
    BodyBuf[BodySize] = '\0';
    AxlHttpClientResponseFree(Response);

    // Parse JSON array
    JSON_CTX Ctx;
    Status = JsonParse(BodyBuf, BodySize, &Ctx);
    if (EFI_ERROR(Status)) return EFI_DEVICE_ERROR;

    // Find the root array (should be token 0)
    if (Ctx.TokenCount == 0 || Ctx.Tokens[0].Type != JSON_TOKEN_ARRAY_START) {
        return EFI_DEVICE_ERROR;
    }

    DIR_ENTRY TempEntries[DIR_CACHE_MAX_ENTRIES];
    UINTN Count = 0;

    JSON_ARRAY_ITER Iter;
    Status = JsonArrayFirst(&Ctx, 0, &Iter);
    if (EFI_ERROR(Status)) return EFI_DEVICE_ERROR;

    UINTN ElemIdx;
    while (!EFI_ERROR(JsonArrayNext(&Iter, &ElemIdx)) &&
           Count < DIR_CACHE_MAX_ENTRIES) {
        DIR_ENTRY *E = &TempEntries[Count];
        SetMem(E, sizeof(*E), 0);

        JsonGetString(&Ctx, ElemIdx, "name", E->Name, sizeof(E->Name));
        JsonGetNumber(&Ctx, ElemIdx, "size", (UINTN *)&E->Size);
        JsonGetBool(&Ctx, ElemIdx, "dir", &E->IsDir);
        JsonGetString(&Ctx, ElemIdx, "modified", E->Modified, sizeof(E->Modified));

        if (E->Name[0] != '\0') Count++;
    }

    // Store in cache
    DirCachePut(Private, Path, TempEntries, Count);

    // Return from cache (stable pointers)
    Slot = DirCacheFind(Private, Path);
    if (Slot != NULL) {
        *Entries = Slot->Entries;
        *EntryCount = Slot->EntryCount;
        return EFI_SUCCESS;
    }

    return EFI_DEVICE_ERROR;
}

EFI_STATUS DirCacheLookupEntry(
    IN  WEBDAVFS_PRIVATE  *Private,
    IN  CONST CHAR8       *DirPath,
    IN  CONST CHAR8       *Name,
    OUT DIR_ENTRY         *Entry
) {
    DIR_ENTRY *Entries = NULL;
    UINTN Count = 0;

    EFI_STATUS Status = DirCacheFetch(Private, DirPath, &Entries, &Count);
    if (EFI_ERROR(Status)) return Status;

    for (UINTN i = 0; i < Count; i++) {
        if (AsciiStriCmp(Entries[i].Name, Name) == 0) {
            CopyMem(Entry, &Entries[i], sizeof(DIR_ENTRY));
            return EFI_SUCCESS;
        }
    }

    return EFI_NOT_FOUND;
}
