/** @file
  FileTransferLib — Volume enumeration, streaming file I/O, directory listing.

  Provides file operations on local UEFI volumes for the serve command.
  All I/O is streaming (8 KB chunks) with optional progress callbacks.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef FILE_TRANSFER_LIB_H_
#define FILE_TRANSFER_LIB_H_

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>

#define FT_CHUNK_SIZE    (8 * 1024)
#define FT_MAX_VOLUMES   8

/// Volume descriptor.
typedef struct {
    EFI_HANDLE                         Handle;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL    *Fs;
    CHAR16                             Name[16];
} FT_VOLUME;

/// Progress callback (called per chunk during streaming I/O).
typedef VOID (*FT_PROGRESS_CB)(
    IN UINTN  BytesTransferred,
    IN UINTN  TotalBytes,
    IN VOID   *Context
    );

/// Streaming read context.
typedef struct {
    EFI_FILE_PROTOCOL  *File;
    UINT64             FileSize;
    UINT64             Position;
    FT_PROGRESS_CB     ProgressCb;
    VOID               *ProgressCtx;
} FT_READ_CTX;

/// Streaming write context.
typedef struct {
    EFI_FILE_PROTOCOL  *File;
    UINT64             BytesWritten;
    FT_PROGRESS_CB     ProgressCb;
    VOID               *ProgressCtx;
} FT_WRITE_CTX;

/// Initialize: enumerate all EFI_SIMPLE_FILE_SYSTEM_PROTOCOL handles.
EFI_STATUS EFIAPI FileTransferInit (VOID);

/// Get number of discovered volumes.
UINTN EFIAPI FileTransferGetVolumeCount (VOID);

/// Get volume by index.
EFI_STATUS EFIAPI FileTransferGetVolume (IN UINTN Index, OUT FT_VOLUME *Volume);

/// Find volume by name (e.g., L"fs0").
EFI_STATUS EFIAPI FileTransferFindVolume (IN CONST CHAR16 *Name, OUT FT_VOLUME *Volume);

/// List volumes as JSON or HTML.
EFI_STATUS EFIAPI FileTransferListVolumes (IN BOOLEAN AsJson, OUT CHAR8 *Buffer, IN UINTN BufferSize, OUT UINTN *Written);

/// List directory contents as JSON or HTML.
EFI_STATUS EFIAPI FileTransferListDir (IN FT_VOLUME *Volume, IN CONST CHAR16 *Path, IN BOOLEAN AsJson, OUT CHAR8 *Buffer, IN UINTN BufferSize, OUT UINTN *Written);

/// Open a file for streaming read, starting at Offset.
EFI_STATUS EFIAPI FileTransferOpenRead (IN FT_VOLUME *Volume, IN CONST CHAR16 *Path, IN UINT64 Offset, IN FT_PROGRESS_CB Cb OPTIONAL, IN VOID *Ctx OPTIONAL, OUT FT_READ_CTX *ReadCtx);

/// Read next chunk (up to BufferSize bytes). *BytesRead == 0 means EOF.
EFI_STATUS EFIAPI FileTransferReadChunk (IN OUT FT_READ_CTX *Ctx, OUT VOID *Buffer, IN UINTN BufferSize, OUT UINTN *BytesRead);

/// Close read context.
VOID EFIAPI FileTransferCloseRead (IN FT_READ_CTX *Ctx);

/// Open a file for streaming write (create/overwrite).
EFI_STATUS EFIAPI FileTransferOpenWrite (IN FT_VOLUME *Volume, IN CONST CHAR16 *Path, IN FT_PROGRESS_CB Cb OPTIONAL, IN VOID *Ctx OPTIONAL, OUT FT_WRITE_CTX *WriteCtx);

/// Write a chunk of data.
EFI_STATUS EFIAPI FileTransferWriteChunk (IN OUT FT_WRITE_CTX *Ctx, IN CONST VOID *Data, IN UINTN Len);

/// Close write context.
VOID EFIAPI FileTransferCloseWrite (IN FT_WRITE_CTX *Ctx);

/// Delete a file or empty directory.
EFI_STATUS EFIAPI FileTransferDelete (IN FT_VOLUME *Volume, IN CONST CHAR16 *Path);

/// Create a directory (with parents).
EFI_STATUS EFIAPI FileTransferMkdir (IN FT_VOLUME *Volume, IN CONST CHAR16 *Path);

/// Get file size.
EFI_STATUS EFIAPI FileTransferGetFileSize (IN FT_VOLUME *Volume, IN CONST CHAR16 *Path, OUT UINT64 *Size);

/// Check if path is a directory.
EFI_STATUS EFIAPI FileTransferIsDir (IN FT_VOLUME *Volume, IN CONST CHAR16 *Path, OUT BOOLEAN *IsDir);

#endif // FILE_TRANSFER_LIB_H_
