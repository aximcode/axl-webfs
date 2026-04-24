/** @file
  axl-webfs-dxe -- Internal types and declarations.

  Private data structures for the remote filesystem driver.
  Uses SIGNATURE_32 + CR macros for container derivation.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef WEBFS_INTERNAL_H_
#define WEBFS_INTERNAL_H_

#include <axl.h>
#include <axl/axl-json.h>
#include <axl/axl-net.h>
#include <axl/axl-log.h>
#include <axl/axl-url.h>
#include <uefi/axl-uefi.h>


#include "net/network.h"

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
#define DEFAULT_SERVER_PORT          8080

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

/// A cached directory listing.
typedef struct {
    char       path[MAX_PATH_LEN];
    uint64_t   timestamp_ms;
    DirEntry   entries[DIR_CACHE_MAX_ENTRIES];
    size_t     entry_count;
    bool       valid;
} DirCacheSlot;

// ---------------------------------------------------------------------------
// Driver private context (one per mount)
// ---------------------------------------------------------------------------

typedef struct {
    UINT32                              signature;
    void                               *image_handle;
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

    /// Directory cache.
    DirCacheSlot                        dir_cache[DIR_CACHE_MAX_SLOTS];
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
} WebFsFileCtx;

#define WEBFS_FILE_FROM_FILE_PROTOCOL(a) \
    AXL_CONTAINER_OF(a, WebFsFileCtx, file)

// ---------------------------------------------------------------------------
// Forward declarations -- webfs.c
// ---------------------------------------------------------------------------

EFI_STATUS EFIAPI DriverEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
EFI_STATUS EFIAPI WebFsDriverUnload(EFI_HANDLE ImageHandle);
EFI_STATUS EFIAPI WebFsOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);

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

#endif // WEBFS_INTERNAL_H_
