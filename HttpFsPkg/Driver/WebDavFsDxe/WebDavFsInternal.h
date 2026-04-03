/** @file
  WebDavFsDxe -- Internal types and declarations (axl-cc port).

  Private data structures for the remote filesystem driver.
  Uses SIGNATURE_32 + CR macros for container derivation.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef WEBDAVFS_INTERNAL_H_
#define WEBDAVFS_INTERNAL_H_

#include <axl.h>
#include <axl/axl-json.h>
#include <axl/axl-net.h>
#include <axl/axl-log.h>
#include <uefi/axl-uefi.h>


#include <Library/NetworkLib.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define WEBDAVFS_PRIVATE_SIGNATURE   AXL_SIGNATURE_32('W','D','F','S')
#define WEBDAVFS_FILE_SIGNATURE      AXL_SIGNATURE_32('W','D','F','L')

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
    char       Name[256];
    uint64_t   Size;
    bool       IsDir;
    char       Modified[32];   ///< ISO 8601 timestamp from server.
} DirEntry;

/// A cached directory listing.
typedef struct {
    char       Path[MAX_PATH_LEN];
    uint64_t   TimestampMs;
    DirEntry   Entries[DIR_CACHE_MAX_ENTRIES];
    size_t     EntryCount;
    bool       Valid;
} DirCacheSlot;

// ---------------------------------------------------------------------------
// Driver private context (one per mount)
// ---------------------------------------------------------------------------

typedef struct {
    UINT32                              Signature;
    EFI_HANDLE                          ImageHandle;
    EFI_HANDLE                          FsHandle;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     SimpleFs;
    EFI_DEVICE_PATH_PROTOCOL            *DevicePath;

    /// Server connection.
    AxlHttpClient                       *HttpClient;
    EFI_IPv4_ADDRESS                    ServerAddr;
    UINT16                              ServerPort;
    char                                BasePath[256];
    char                                BaseUrl[280];
    bool                                ReadOnly;

    /// Directory cache.
    DirCacheSlot                        DirCache[DIR_CACHE_MAX_SLOTS];
} WEBDAVFS_PRIVATE;

#define WEBDAVFS_PRIVATE_FROM_SIMPLE_FS(a) \
    AXL_CONTAINER_OF(a, WEBDAVFS_PRIVATE, SimpleFs)

// ---------------------------------------------------------------------------
// Per-file-handle context (one per Open)
// ---------------------------------------------------------------------------

typedef struct {
    UINT32               Signature;
    EFI_FILE_PROTOCOL    File;
    WEBDAVFS_PRIVATE     *Private;

    char                 Path[MAX_PATH_LEN];
    bool                 IsDir;
    bool                 IsRoot;
    uint64_t             FileSize;
    uint64_t             Position;

    /// Directory iteration state.
    DirEntry             *DirEntries;
    size_t               DirEntryCount;
    size_t               DirReadIndex;
    bool                 DirLoaded;

    /// Read-ahead buffer (allocated for files, NULL for dirs).
    uint8_t              *ReadAheadBuf;
    uint64_t             ReadAheadStart;
    size_t               ReadAheadLen;
} WEBDAVFS_FILE;

#define WEBDAVFS_FILE_FROM_FILE_PROTOCOL(a) \
    AXL_CONTAINER_OF(a, WEBDAVFS_FILE, File)

// ---------------------------------------------------------------------------
// Forward declarations -- WebDavFs.c
// ---------------------------------------------------------------------------

EFI_STATUS EFIAPI DriverEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
EFI_STATUS EFIAPI WebDavFsDriverUnload(EFI_HANDLE ImageHandle);
EFI_STATUS EFIAPI WebDavFsOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);

// ---------------------------------------------------------------------------
// Forward declarations -- WebDavFsFile.c
// ---------------------------------------------------------------------------

EFI_STATUS EFIAPI WebDavFsOpen(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
EFI_STATUS EFIAPI WebDavFsClose(EFI_FILE_PROTOCOL *This);
EFI_STATUS EFIAPI WebDavFsDelete(EFI_FILE_PROTOCOL *This);
EFI_STATUS EFIAPI WebDavFsRead(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsWrite(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsGetPosition(EFI_FILE_PROTOCOL *This, UINT64 *Position);
EFI_STATUS EFIAPI WebDavFsSetPosition(EFI_FILE_PROTOCOL *This, UINT64 Position);
EFI_STATUS EFIAPI WebDavFsGetInfo(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsSetInfo(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN BufferSize, VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsFlush(EFI_FILE_PROTOCOL *This);

/// Allocate and initialize a WEBDAVFS_FILE with all function pointers wired.
WEBDAVFS_FILE * WebDavFsCreateFileHandle(WEBDAVFS_PRIVATE *Private, const char *Path, bool IsDir, uint64_t FileSize);

// ---------------------------------------------------------------------------
// Forward declarations -- WebDavFsCache.c
// ---------------------------------------------------------------------------

int  DirCacheFetch(WEBDAVFS_PRIVATE *Private, const char *Path, DirEntry **Entries, size_t *EntryCount);
int  DirCacheLookupEntry(WEBDAVFS_PRIVATE *Private, const char *DirPath, const char *Name, DirEntry *Entry);
void DirCacheInvalidate(WEBDAVFS_PRIVATE *Private, const char *Path);

/// Issue an HTTP request with automatic reconnect on connection error.
/// Caller must free *Response with axl_http_client_response_free().
int WebDavFsHttpRequest(WEBDAVFS_PRIVATE *Private, const char *Method, const char *Path, AxlHashTable *ExtraHeaders, const void *Body, size_t BodyLen, AxlHttpClientResponse **Response);

#endif // WEBDAVFS_INTERNAL_H_
