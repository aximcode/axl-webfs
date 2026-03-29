/** @file
  WebDavFsDxe — Internal types and declarations.

  Private data structures for the remote filesystem driver.
  Uses standard EDK2 CR() macro + SIGNATURE_32 for container derivation.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef WEBDAVFS_INTERNAL_H_
#define WEBDAVFS_INTERNAL_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/DevicePathLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>

#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>

#include <Library/NetworkLib.h>
#include <Library/JsonLib.h>
#include <Library/AxlLib.h>
#include <axl/axl-net.h>

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------

#define WEBDAVFS_PRIVATE_SIGNATURE   SIGNATURE_32('W','D','F','S')
#define WEBDAVFS_FILE_SIGNATURE      SIGNATURE_32('W','D','F','L')

#define DIR_CACHE_TTL_MS             2000     ///< 2 second directory cache TTL.
#define DIR_CACHE_MAX_SLOTS          16       ///< Maximum cached directories.
#define DIR_CACHE_MAX_ENTRIES        64       ///< Max files per cached directory.
#define READAHEAD_BUF_SIZE           (64 * 1024)
#define MAX_PATH_LEN                 512
#define HTTP_BODY_BUF_SIZE           4096
#define DEFAULT_SERVER_PORT          8080

#define VOLUME_LABEL                 L"XferMount"

// ----------------------------------------------------------------------------
// Directory cache types
// ----------------------------------------------------------------------------

/// A single file or directory entry from a JSON listing.
typedef struct {
    CHAR8      Name[256];
    UINT64     Size;
    BOOLEAN    IsDir;
    CHAR8      Modified[32];   ///< ISO 8601 timestamp from server.
} DIR_ENTRY;

/// A cached directory listing.
typedef struct {
    CHAR8      Path[MAX_PATH_LEN];
    UINT64     TimestampMs;
    DIR_ENTRY  Entries[DIR_CACHE_MAX_ENTRIES];
    UINTN      EntryCount;
    BOOLEAN    Valid;
} DIR_CACHE_SLOT;

// ----------------------------------------------------------------------------
// Driver private context (one per mount)
// ----------------------------------------------------------------------------

typedef struct {
    UINT32                              Signature;
    EFI_HANDLE                          ImageHandle;
    EFI_HANDLE                          FsHandle;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     SimpleFs;
    EFI_DEVICE_PATH_PROTOCOL            *DevicePath;

    /// Server connection.
    AxlHttpClient                     *HttpClient;
    EFI_IPv4_ADDRESS                    ServerAddr;
    UINT16                              ServerPort;
    CHAR8                               BasePath[256];
    CHAR8                               BaseUrl[280];
    BOOLEAN                             ReadOnly;

    /// Directory cache.
    DIR_CACHE_SLOT                      DirCache[DIR_CACHE_MAX_SLOTS];
} WEBDAVFS_PRIVATE;

#define WEBDAVFS_PRIVATE_FROM_SIMPLE_FS(a) \
    CR(a, WEBDAVFS_PRIVATE, SimpleFs, WEBDAVFS_PRIVATE_SIGNATURE)

// ----------------------------------------------------------------------------
// Per-file-handle context (one per Open)
// ----------------------------------------------------------------------------

typedef struct {
    UINT32               Signature;
    EFI_FILE_PROTOCOL    File;
    WEBDAVFS_PRIVATE     *Private;

    CHAR8                Path[MAX_PATH_LEN];
    BOOLEAN              IsDir;
    BOOLEAN              IsRoot;
    UINT64               FileSize;
    UINT64               Position;

    /// Directory iteration state.
    DIR_ENTRY            *DirEntries;
    UINTN                DirEntryCount;
    UINTN                DirReadIndex;
    BOOLEAN              DirLoaded;

    /// Read-ahead buffer (allocated for files, NULL for dirs).
    UINT8                *ReadAheadBuf;
    UINT64               ReadAheadStart;
    UINTN                ReadAheadLen;
} WEBDAVFS_FILE;

#define WEBDAVFS_FILE_FROM_FILE_PROTOCOL(a) \
    CR(a, WEBDAVFS_FILE, File, WEBDAVFS_FILE_SIGNATURE)

// ----------------------------------------------------------------------------
// Forward declarations — WebDavFs.c
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI WebDavFsDriverEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable);
EFI_STATUS EFIAPI WebDavFsDriverUnload (IN EFI_HANDLE ImageHandle);
EFI_STATUS EFIAPI WebDavFsOpenVolume (IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, OUT EFI_FILE_PROTOCOL **Root);

// ----------------------------------------------------------------------------
// Forward declarations — WebDavFsFile.c
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI WebDavFsOpen (IN EFI_FILE_PROTOCOL *This, OUT EFI_FILE_PROTOCOL **NewHandle, IN CHAR16 *FileName, IN UINT64 OpenMode, IN UINT64 Attributes);
EFI_STATUS EFIAPI WebDavFsClose (IN EFI_FILE_PROTOCOL *This);
EFI_STATUS EFIAPI WebDavFsDelete (IN EFI_FILE_PROTOCOL *This);
EFI_STATUS EFIAPI WebDavFsRead (IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize, OUT VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsWrite (IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize, IN VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsGetPosition (IN EFI_FILE_PROTOCOL *This, OUT UINT64 *Position);
EFI_STATUS EFIAPI WebDavFsSetPosition (IN EFI_FILE_PROTOCOL *This, IN UINT64 Position);
EFI_STATUS EFIAPI WebDavFsGetInfo (IN EFI_FILE_PROTOCOL *This, IN EFI_GUID *InformationType, IN OUT UINTN *BufferSize, OUT VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsSetInfo (IN EFI_FILE_PROTOCOL *This, IN EFI_GUID *InformationType, IN UINTN BufferSize, IN VOID *Buffer);
EFI_STATUS EFIAPI WebDavFsFlush (IN EFI_FILE_PROTOCOL *This);

/// Allocate and initialize a WEBDAVFS_FILE with all function pointers wired.
WEBDAVFS_FILE * WebDavFsCreateFileHandle (IN WEBDAVFS_PRIVATE *Private, IN CONST CHAR8 *Path, IN BOOLEAN IsDir, IN UINT64 FileSize);

// ----------------------------------------------------------------------------
// Forward declarations — WebDavFsCache.c
// ----------------------------------------------------------------------------

EFI_STATUS DirCacheFetch (IN WEBDAVFS_PRIVATE *Private, IN CONST CHAR8 *Path, OUT DIR_ENTRY **Entries, OUT UINTN *EntryCount);
EFI_STATUS DirCacheLookupEntry (IN WEBDAVFS_PRIVATE *Private, IN CONST CHAR8 *DirPath, IN CONST CHAR8 *Name, OUT DIR_ENTRY *Entry);
VOID DirCacheInvalidate (IN WEBDAVFS_PRIVATE *Private, IN CONST CHAR8 *Path);

/// Issue an HTTP request with automatic reconnect on connection error.
/// Caller must free *Response with axl_http_client_response_free().
EFI_STATUS WebDavFsHttpRequest (IN WEBDAVFS_PRIVATE *Private, IN CONST CHAR8 *Method, IN CONST CHAR8 *Path, IN AXL_HASH_TABLE *ExtraHeaders OPTIONAL, IN CONST VOID *Body OPTIONAL, IN UINTN BodyLen, OUT AxlHttpClientResponse **Response);

#endif // WEBDAVFS_INTERNAL_H_
