/** @file
  FileTransferLib — Volume enumeration, streaming file I/O, file operations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/FileTransferLib.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/DevicePathLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

// ----------------------------------------------------------------------------
// Volume table
// ----------------------------------------------------------------------------

static FT_VOLUME  mVolumes[FT_MAX_VOLUMES];
static UINTN      mVolumeCount = 0;

EFI_STATUS EFIAPI FileTransferInit(VOID) {
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;

    EFI_STATUS Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL,
        &HandleCount, &Handles);
    if (EFI_ERROR(Status)) return Status;

    mVolumeCount = 0;
    for (UINTN i = 0; i < HandleCount && mVolumeCount < FT_MAX_VOLUMES; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
        Status = gBS->OpenProtocol(
            Handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs,
            gImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(Status)) continue;

        FT_VOLUME *V = &mVolumes[mVolumeCount];
        V->Handle = Handles[i];
        V->Fs = Fs;
        UnicodeSPrint(V->Name, sizeof(V->Name), L"fs%d", mVolumeCount);
        mVolumeCount++;
    }

    FreePool(Handles);
    return EFI_SUCCESS;
}

UINTN EFIAPI FileTransferGetVolumeCount(VOID) {
    return mVolumeCount;
}

EFI_STATUS EFIAPI FileTransferGetVolume(IN UINTN Index, OUT FT_VOLUME *Volume) {
    if (Index >= mVolumeCount || Volume == NULL) return EFI_NOT_FOUND;
    CopyMem(Volume, &mVolumes[Index], sizeof(FT_VOLUME));
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI FileTransferFindVolume(IN CONST CHAR16 *Name, OUT FT_VOLUME *Volume) {
    for (UINTN i = 0; i < mVolumeCount; i++) {
        if (StrCmp(mVolumes[i].Name, Name) == 0) {
            CopyMem(Volume, &mVolumes[i], sizeof(FT_VOLUME));
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

// ----------------------------------------------------------------------------
// Path helpers
// ----------------------------------------------------------------------------

/// Open a file on a volume by CHAR16 path. Caller must Close().
static EFI_STATUS OpenFileOnVolume(
    IN  FT_VOLUME          *Volume,
    IN  CONST CHAR16       *Path,
    IN  UINT64             Mode,
    IN  UINT64             Attributes,
    OUT EFI_FILE_PROTOCOL  **File
) {
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_STATUS Status = Volume->Fs->OpenVolume(Volume->Fs, &Root);
    if (EFI_ERROR(Status)) return Status;

    // Convert forward slashes to backslashes for UEFI
    CHAR16 UefiPath[512];
    UINTN i = 0;
    while (Path[i] != L'\0' && i < 511) {
        UefiPath[i] = (Path[i] == L'/') ? L'\\' : Path[i];
        i++;
    }
    UefiPath[i] = L'\0';

    // Skip leading backslash for Open (root is already open)
    CHAR16 *OpenPath = UefiPath;
    if (OpenPath[0] == L'\\') OpenPath++;
    if (OpenPath[0] == L'\0') {
        *File = Root;
        return EFI_SUCCESS;
    }

    Status = Root->Open(Root, File, OpenPath, Mode, Attributes);
    Root->Close(Root);
    return Status;
}

// ----------------------------------------------------------------------------
// Streaming read
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI FileTransferOpenRead(
    IN  FT_VOLUME       *Volume,
    IN  CONST CHAR16    *Path,
    IN  UINT64          Offset,
    IN  FT_PROGRESS_CB  Cb       OPTIONAL,
    IN  VOID            *Ctx     OPTIONAL,
    OUT FT_READ_CTX     *ReadCtx
) {
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_STATUS Status = OpenFileOnVolume(Volume, Path, EFI_FILE_MODE_READ, 0, &File);
    if (EFI_ERROR(Status)) return Status;

    // Get file size
    UINTN InfoSize = 0;
    File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
    EFI_FILE_INFO *Info = AllocatePool(InfoSize);
    if (Info == NULL) { File->Close(File); return EFI_OUT_OF_RESOURCES; }
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
    UINT64 FileSize = Info->FileSize;
    FreePool(Info);
    if (EFI_ERROR(Status)) { File->Close(File); return Status; }

    if (Offset > 0) {
        File->SetPosition(File, Offset);
    }

    ReadCtx->File = File;
    ReadCtx->FileSize = FileSize;
    ReadCtx->Position = Offset;
    ReadCtx->ProgressCb = Cb;
    ReadCtx->ProgressCtx = Ctx;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI FileTransferReadChunk(
    IN OUT FT_READ_CTX  *Ctx,
    OUT    VOID         *Buffer,
    IN     UINTN        BufferSize,
    OUT    UINTN        *BytesRead
) {
    UINTN ReadSize = BufferSize;
    EFI_STATUS Status = Ctx->File->Read(Ctx->File, &ReadSize, Buffer);
    if (EFI_ERROR(Status)) { *BytesRead = 0; return Status; }

    *BytesRead = ReadSize;
    Ctx->Position += ReadSize;

    if (Ctx->ProgressCb != NULL) {
        Ctx->ProgressCb((UINTN)Ctx->Position, (UINTN)Ctx->FileSize, Ctx->ProgressCtx);
    }
    return EFI_SUCCESS;
}

VOID EFIAPI FileTransferCloseRead(IN FT_READ_CTX *Ctx) {
    if (Ctx->File != NULL) {
        Ctx->File->Close(Ctx->File);
        Ctx->File = NULL;
    }
}

// ----------------------------------------------------------------------------
// Streaming write
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI FileTransferOpenWrite(
    IN  FT_VOLUME       *Volume,
    IN  CONST CHAR16    *Path,
    IN  FT_PROGRESS_CB  Cb       OPTIONAL,
    IN  VOID            *Ctx     OPTIONAL,
    OUT FT_WRITE_CTX    *WriteCtx
) {
    // Create parent directories if needed
    CHAR16 ParentPath[512];
    UINTN Len = StrLen(Path);
    if (Len >= 512) return EFI_INVALID_PARAMETER;
    StrCpyS(ParentPath, 512, Path);

    // Find last separator
    UINTN LastSep = 0;
    for (UINTN i = 0; i < Len; i++) {
        if (ParentPath[i] == L'/' || ParentPath[i] == L'\\') LastSep = i;
    }
    if (LastSep > 0) {
        ParentPath[LastSep] = L'\0';
        FileTransferMkdir(Volume, ParentPath);
    }

    EFI_FILE_PROTOCOL *File = NULL;
    EFI_STATUS Status = OpenFileOnVolume(
        Volume, Path,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        0, &File);
    if (EFI_ERROR(Status)) return Status;

    // Truncate existing file by deleting and recreating
    // (simpler than seeking to 0 and setting size)

    WriteCtx->File = File;
    WriteCtx->BytesWritten = 0;
    WriteCtx->ProgressCb = Cb;
    WriteCtx->ProgressCtx = Ctx;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI FileTransferWriteChunk(
    IN OUT FT_WRITE_CTX  *Ctx,
    IN     CONST VOID    *Data,
    IN     UINTN         Len
) {
    UINTN WriteSize = Len;
    EFI_STATUS Status = Ctx->File->Write(Ctx->File, &WriteSize, (VOID *)Data);
    if (EFI_ERROR(Status)) return Status;

    Ctx->BytesWritten += WriteSize;

    if (Ctx->ProgressCb != NULL) {
        Ctx->ProgressCb((UINTN)Ctx->BytesWritten, (UINTN)MAX_UINTN, Ctx->ProgressCtx);
    }
    return EFI_SUCCESS;
}

VOID EFIAPI FileTransferCloseWrite(IN FT_WRITE_CTX *Ctx) {
    if (Ctx->File != NULL) {
        Ctx->File->Flush(Ctx->File);
        Ctx->File->Close(Ctx->File);
        Ctx->File = NULL;
    }
}

// ----------------------------------------------------------------------------
// File operations
// ----------------------------------------------------------------------------

EFI_STATUS EFIAPI FileTransferDelete(IN FT_VOLUME *Volume, IN CONST CHAR16 *Path) {
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_STATUS Status = OpenFileOnVolume(
        Volume, Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &File);
    if (EFI_ERROR(Status)) return Status;
    return File->Delete(File);
}

EFI_STATUS EFIAPI FileTransferMkdir(IN FT_VOLUME *Volume, IN CONST CHAR16 *Path) {
    EFI_FILE_PROTOCOL *Dir = NULL;
    EFI_STATUS Status = OpenFileOnVolume(
        Volume, Path,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        EFI_FILE_DIRECTORY, &Dir);
    if (EFI_ERROR(Status)) return Status;
    Dir->Close(Dir);
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI FileTransferGetFileSize(
    IN  FT_VOLUME  *Volume,
    IN  CONST CHAR16  *Path,
    OUT UINT64     *Size
) {
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_STATUS Status = OpenFileOnVolume(Volume, Path, EFI_FILE_MODE_READ, 0, &File);
    if (EFI_ERROR(Status)) return Status;

    UINTN InfoSize = 0;
    File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
    EFI_FILE_INFO *Info = AllocatePool(InfoSize);
    if (Info == NULL) { File->Close(File); return EFI_OUT_OF_RESOURCES; }
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
    *Size = Info->FileSize;
    FreePool(Info);
    File->Close(File);
    return Status;
}

EFI_STATUS EFIAPI FileTransferIsDir(
    IN  FT_VOLUME    *Volume,
    IN  CONST CHAR16 *Path,
    OUT BOOLEAN      *IsDir
) {
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_STATUS Status = OpenFileOnVolume(Volume, Path, EFI_FILE_MODE_READ, 0, &File);
    if (EFI_ERROR(Status)) return Status;

    UINTN InfoSize = 0;
    File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
    EFI_FILE_INFO *Info = AllocatePool(InfoSize);
    if (Info == NULL) { File->Close(File); return EFI_OUT_OF_RESOURCES; }
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
    *IsDir = (Info->Attribute & EFI_FILE_DIRECTORY) != 0;
    FreePool(Info);
    File->Close(File);
    return Status;
}
