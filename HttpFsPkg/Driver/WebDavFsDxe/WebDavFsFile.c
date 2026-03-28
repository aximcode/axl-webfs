/** @file
  WebDavFsDxe — EFI_FILE_PROTOCOL implementation.

  Every UEFI file operation translates to HTTP requests against
  xfer-server.py. Directory listings use GET /list/, file I/O uses
  GET/PUT /files/, with a 64 KB read-ahead buffer for sequential reads.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "WebDavFsInternal.h"
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>

// ----------------------------------------------------------------------------
// Path helpers
// ----------------------------------------------------------------------------

/// Get the parent directory path. E.g., "/foo/bar/" -> "/foo/", "/foo/bar.txt" -> "/foo/"
static VOID GetParentPath(IN CONST CHAR8 *Path, OUT CHAR8 *Parent, IN UINTN ParentSize) {
    UINTN Len = AsciiStrLen(Path);

    // Strip trailing slash for processing
    if (Len > 1 && Path[Len - 1] == '/') Len--;

    // Find last slash
    UINTN LastSlash = 0;
    for (UINTN i = 0; i < Len; i++) {
        if (Path[i] == '/') LastSlash = i;
    }

    if (LastSlash == 0) {
        AsciiStrCpyS(Parent, ParentSize, "/");
    } else {
        UINTN CopyLen = LastSlash + 1;
        if (CopyLen >= ParentSize) CopyLen = ParentSize - 1;
        CopyMem(Parent, Path, CopyLen);
        Parent[CopyLen] = '\0';
    }
}

/// Get the filename from a path. E.g., "/foo/bar.txt" -> "bar.txt"
static CONST CHAR8 * GetFileName(IN CONST CHAR8 *Path) {
    UINTN Len = AsciiStrLen(Path);
    if (Len > 1 && Path[Len - 1] == '/') Len--;

    for (UINTN i = Len; i > 0; i--) {
        if (Path[i - 1] == '/') return &Path[i];
    }
    return Path;
}

/// Resolve a CHAR16 filename relative to a base CHAR8 path.
/// Handles \, ., .., and converts to forward slashes.
static EFI_STATUS ResolvePath(
    IN  CONST CHAR8   *BasePath,
    IN  CONST CHAR16  *FileName,
    OUT CHAR8         *Resolved,
    IN  UINTN         ResolvedSize
) {
    // Convert FileName to ASCII
    CHAR8 Name8[MAX_PATH_LEN];
    UINTN i = 0;
    while (FileName[i] != L'\0' && i < MAX_PATH_LEN - 1) {
        CHAR16 Ch = FileName[i];
        Name8[i] = (Ch == L'\\') ? '/' : (CHAR8)Ch;
        i++;
    }
    Name8[i] = '\0';

    // Absolute path (starts with /)
    if (Name8[0] == '/') {
        AsciiStrCpyS(Resolved, ResolvedSize, Name8);
    } else {
        // Relative — append to base
        AsciiStrCpyS(Resolved, ResolvedSize, BasePath);
        UINTN BaseLen = AsciiStrLen(Resolved);
        if (BaseLen > 0 && Resolved[BaseLen - 1] != '/') {
            AsciiStrCatS(Resolved, ResolvedSize, "/");
        }
        AsciiStrCatS(Resolved, ResolvedSize, Name8);
    }

    // Resolve . and .. components in place
    // Simple approach: rebuild path component by component
    CHAR8 Temp[MAX_PATH_LEN];
    UINTN TempPos = 0;
    CHAR8 *P = Resolved;

    if (*P == '/') {
        Temp[TempPos++] = '/';
        P++;
    }

    while (*P != '\0' && TempPos < MAX_PATH_LEN - 1) {
        // Skip consecutive slashes
        if (*P == '/') { P++; continue; }

        // Find end of component
        CHAR8 *CompStart = P;
        while (*P != '/' && *P != '\0') P++;
        UINTN CompLen = (UINTN)(P - CompStart);
        if (*P == '/') P++;

        if (CompLen == 1 && CompStart[0] == '.') {
            continue;  // Skip "."
        }

        if (CompLen == 2 && CompStart[0] == '.' && CompStart[1] == '.') {
            // Go up one level
            if (TempPos > 1) {
                TempPos--;  // Back over trailing slash
                while (TempPos > 1 && Temp[TempPos - 1] != '/') TempPos--;
            }
            continue;
        }

        // Copy component
        if (TempPos + CompLen + 1 >= MAX_PATH_LEN) break;
        CopyMem(Temp + TempPos, CompStart, CompLen);
        TempPos += CompLen;
        Temp[TempPos++] = '/';
    }

    // Remove trailing slash unless it's the root
    if (TempPos > 1 && Temp[TempPos - 1] == '/') TempPos--;
    Temp[TempPos] = '\0';

    AsciiStrCpyS(Resolved, ResolvedSize, Temp);
    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// File handle creation
// ----------------------------------------------------------------------------

WEBDAVFS_FILE * WebDavFsCreateFileHandle(
    IN WEBDAVFS_PRIVATE  *Private,
    IN CONST CHAR8       *Path,
    IN BOOLEAN           IsDir,
    IN UINT64            FileSize
) {
    WEBDAVFS_FILE *F = AllocateZeroPool(sizeof(WEBDAVFS_FILE));
    if (F == NULL) return NULL;

    F->Signature = WEBDAVFS_FILE_SIGNATURE;
    F->Private = Private;
    AsciiStrCpyS(F->Path, MAX_PATH_LEN, Path);
    F->IsDir = IsDir;
    F->FileSize = FileSize;

    F->File.Revision = EFI_FILE_PROTOCOL_REVISION;
    F->File.Open = WebDavFsOpen;
    F->File.Close = WebDavFsClose;
    F->File.Delete = WebDavFsDelete;
    F->File.Read = WebDavFsRead;
    F->File.Write = WebDavFsWrite;
    F->File.GetPosition = WebDavFsGetPosition;
    F->File.SetPosition = WebDavFsSetPosition;
    F->File.GetInfo = WebDavFsGetInfo;
    F->File.SetInfo = WebDavFsSetInfo;
    F->File.Flush = WebDavFsFlush;

    // Allocate read-ahead buffer for files
    if (!IsDir) {
        F->ReadAheadBuf = AllocatePool(READAHEAD_BUF_SIZE);
        // NULL is OK — we'll just skip read-ahead
    }

    return F;
}

// ----------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Open
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsOpen (
    IN  EFI_FILE_PROTOCOL   *This,
    OUT EFI_FILE_PROTOCOL   **NewHandle,
    IN  CHAR16              *FileName,
    IN  UINT64              OpenMode,
    IN  UINT64              Attributes
    )
{
    WEBDAVFS_FILE *Self = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = Self->Private;

    // Handle opening "." — return a new handle to the same path
    if (StrCmp(FileName, L".") == 0 || StrCmp(FileName, L"") == 0) {
        WEBDAVFS_FILE *New = WebDavFsCreateFileHandle(
            Private, Self->Path, Self->IsDir, Self->FileSize);
        if (New == NULL) return EFI_OUT_OF_RESOURCES;
        New->IsRoot = Self->IsRoot;
        *NewHandle = &New->File;
        return EFI_SUCCESS;
    }

    // Resolve the target path
    CHAR8 Resolved[MAX_PATH_LEN];
    EFI_STATUS Status = ResolvePath(Self->Path, FileName, Resolved, sizeof(Resolved));
    if (EFI_ERROR(Status)) return Status;

    // Look up the entry in the parent directory
    CHAR8 ParentDir[MAX_PATH_LEN];
    GetParentPath(Resolved, ParentDir, sizeof(ParentDir));
    CONST CHAR8 *EntryName = GetFileName(Resolved);

    DIR_ENTRY Entry;
    Status = DirCacheLookupEntry(Private, ParentDir, EntryName, &Entry);

    if (EFI_ERROR(Status)) {
        // Not found — check if CREATE mode
        if (!(OpenMode & EFI_FILE_MODE_CREATE)) {
            return EFI_NOT_FOUND;
        }

        // Create file or directory
        if (Attributes & EFI_FILE_DIRECTORY) {
            // POST /files/<path>?mkdir
            CHAR8 MkdirPath[MAX_PATH_LEN];
            AsciiSPrint(MkdirPath, sizeof(MkdirPath), "/files%a?mkdir", Resolved);

            AXL_HTTP_CLIENT_RESPONSE *Resp = NULL;
            Status = WebDavFsHttpRequest(
                Private, "POST", MkdirPath, NULL, NULL, 0, &Resp);
            if (EFI_ERROR(Status) || Resp == NULL) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;

            UINTN MkdirStatus = Resp->StatusCode;
            AxlHttpClientResponseFree(Resp);
            if (MkdirStatus != 201 && MkdirStatus != 200) {
                return EFI_DEVICE_ERROR;
            }

            DirCacheInvalidate(Private, ParentDir);
            Entry.IsDir = TRUE;
            Entry.Size = 0;
        } else {
            // PUT /files/<path> with empty body
            CHAR8 FilePath[MAX_PATH_LEN];
            AsciiSPrint(FilePath, sizeof(FilePath), "/files%a", Resolved);

            AXL_HTTP_CLIENT_RESPONSE *Resp = NULL;
            Status = WebDavFsHttpRequest(
                Private, "PUT", FilePath, NULL, "", 0, &Resp);
            if (EFI_ERROR(Status) || Resp == NULL) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
            AxlHttpClientResponseFree(Resp);

            DirCacheInvalidate(Private, ParentDir);
            Entry.IsDir = FALSE;
            Entry.Size = 0;
        }
    }

    // Create file handle
    WEBDAVFS_FILE *New = WebDavFsCreateFileHandle(
        Private, Resolved, Entry.IsDir, Entry.Size);
    if (New == NULL) return EFI_OUT_OF_RESOURCES;

    *NewHandle = &New->File;
    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Close
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsClose (
    IN EFI_FILE_PROTOCOL  *This
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (F->ReadAheadBuf != NULL) {
        FreePool(F->ReadAheadBuf);
    }
    if (F->DirEntries != NULL && F->DirLoaded) {
        // DirEntries points into cache — no free needed
    }
    FreePool(F);
    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Read
// ----------------------------------------------------------------------------

/// Read from a file (with read-ahead buffer).
static EFI_STATUS ReadFile(
    IN     WEBDAVFS_FILE  *F,
    IN OUT UINTN          *BufferSize,
    OUT    VOID           *Buffer
) {
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (F->Position >= F->FileSize) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    UINTN Wanted = *BufferSize;
    if (F->Position + Wanted > F->FileSize) {
        Wanted = (UINTN)(F->FileSize - F->Position);
    }

    // Check read-ahead buffer
    if (F->ReadAheadBuf != NULL && F->ReadAheadLen > 0 &&
        F->Position >= F->ReadAheadStart &&
        F->Position < F->ReadAheadStart + F->ReadAheadLen) {
        UINTN Offset = (UINTN)(F->Position - F->ReadAheadStart);
        UINTN Available = F->ReadAheadLen - Offset;
        UINTN ToCopy = (Wanted < Available) ? Wanted : Available;
        CopyMem(Buffer, F->ReadAheadBuf + Offset, ToCopy);
        F->Position += ToCopy;
        *BufferSize = ToCopy;
        return EFI_SUCCESS;
    }

    // Read-ahead miss — fetch from server with read-ahead
    UINTN FetchSize = Wanted;
    if (F->ReadAheadBuf != NULL && FetchSize < READAHEAD_BUF_SIZE) {
        FetchSize = READAHEAD_BUF_SIZE;
    }
    if (F->Position + FetchSize > F->FileSize) {
        FetchSize = (UINTN)(F->FileSize - F->Position);
    }

    // Build Range header
    CHAR8 RangeVal[64];
    AsciiSPrint(RangeVal, sizeof(RangeVal), "bytes=%llu-%llu",
                F->Position, F->Position + FetchSize - 1);
    AXL_HASH_TABLE *RangeHdrs = AxlHashNew();
    if (RangeHdrs == NULL) return EFI_OUT_OF_RESOURCES;
    AxlHashSet(RangeHdrs, "range", RangeVal);

    CHAR8 FilePath[MAX_PATH_LEN];
    AsciiSPrint(FilePath, sizeof(FilePath), "/files%a", F->Path);

    AXL_HTTP_CLIENT_RESPONSE *Resp = NULL;
    EFI_STATUS Status = WebDavFsHttpRequest(
        Private, "GET", FilePath, RangeHdrs, NULL, 0, &Resp);
    AxlHashFree(RangeHdrs);
    if (EFI_ERROR(Status) || Resp == NULL) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
    if (Resp->StatusCode != 200 && Resp->StatusCode != 206) {
        AxlHttpClientResponseFree(Resp);
        *BufferSize = 0;
        return EFI_DEVICE_ERROR;
    }

    // Copy response body into read-ahead buffer or directly into caller's buffer
    UINTN TotalRead = Resp->BodySize;
    if (TotalRead > FetchSize) TotalRead = FetchSize;

    if (F->ReadAheadBuf != NULL) {
        UINTN ToCopy = (TotalRead < READAHEAD_BUF_SIZE) ? TotalRead : READAHEAD_BUF_SIZE;
        CopyMem(F->ReadAheadBuf, Resp->Body, ToCopy);
        F->ReadAheadStart = F->Position;
        F->ReadAheadLen = ToCopy;
        UINTN UserCopy = (Wanted < ToCopy) ? Wanted : ToCopy;
        CopyMem(Buffer, F->ReadAheadBuf, UserCopy);
        F->Position += UserCopy;
        *BufferSize = UserCopy;
    } else {
        UINTN ToCopy = (Wanted < TotalRead) ? Wanted : TotalRead;
        CopyMem(Buffer, Resp->Body, ToCopy);
        F->Position += ToCopy;
        *BufferSize = ToCopy;
    }
    AxlHttpClientResponseFree(Resp);

    return EFI_SUCCESS;
}

/// Build an EFI_FILE_INFO from a DIR_ENTRY, converting name to UCS-2.
static UINTN BuildFileInfo(
    IN  DIR_ENTRY      *Entry,
    OUT EFI_FILE_INFO  *Info,
    IN  UINTN          BufSize
) {
    // Calculate needed size
    UINTN NameLen = AsciiStrLen(Entry->Name);
    UINTN NeededSize = SIZE_OF_EFI_FILE_INFO + (NameLen + 1) * sizeof(CHAR16);

    if (Info == NULL || BufSize < NeededSize) return NeededSize;

    SetMem(Info, NeededSize, 0);
    Info->Size = NeededSize;
    Info->FileSize = Entry->Size;
    Info->PhysicalSize = Entry->Size;
    if (Entry->IsDir) {
        Info->Attribute = EFI_FILE_DIRECTORY;
    }

    // Convert name to UCS-2
    for (UINTN i = 0; i <= NameLen; i++) {
        Info->FileName[i] = (CHAR16)Entry->Name[i];
    }

    return NeededSize;
}

/// Read next directory entry.
static EFI_STATUS ReadDir(
    IN     WEBDAVFS_FILE  *F,
    IN OUT UINTN          *BufferSize,
    OUT    VOID           *Buffer
) {
    WEBDAVFS_PRIVATE *Private = F->Private;

    // Lazy-load directory listing
    if (!F->DirLoaded) {
        EFI_STATUS Status = DirCacheFetch(
            Private, F->Path, &F->DirEntries, &F->DirEntryCount);
        if (EFI_ERROR(Status)) return Status;
        F->DirLoaded = TRUE;
        F->DirReadIndex = 0;
    }

    // End of directory
    if (F->DirReadIndex >= F->DirEntryCount) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    DIR_ENTRY *Entry = &F->DirEntries[F->DirReadIndex];
    UINTN Needed = BuildFileInfo(Entry, NULL, 0);

    if (*BufferSize < Needed) {
        *BufferSize = Needed;
        return EFI_BUFFER_TOO_SMALL;
    }

    BuildFileInfo(Entry, (EFI_FILE_INFO *)Buffer, *BufferSize);
    *BufferSize = Needed;
    F->DirReadIndex++;
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsRead (
    IN     EFI_FILE_PROTOCOL  *This,
    IN OUT UINTN              *BufferSize,
    OUT    VOID               *Buffer
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (F->IsDir) {
        return ReadDir(F, BufferSize, Buffer);
    } else {
        return ReadFile(F, BufferSize, Buffer);
    }
}

// ----------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Write
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsWrite (
    IN     EFI_FILE_PROTOCOL  *This,
    IN OUT UINTN              *BufferSize,
    IN     VOID               *Buffer
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (F->IsDir) return EFI_UNSUPPORTED;
    if (Private->ReadOnly) return EFI_WRITE_PROTECTED;

    CHAR8 FilePath[MAX_PATH_LEN];
    AsciiSPrint(FilePath, sizeof(FilePath), "/files%a", F->Path);

    AXL_HTTP_CLIENT_RESPONSE *Resp = NULL;
    EFI_STATUS Status = WebDavFsHttpRequest(
        Private, "PUT", FilePath, NULL, Buffer, *BufferSize, &Resp);
    if (EFI_ERROR(Status) || Resp == NULL) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;

    UINTN WriteStatus = Resp->StatusCode;
    AxlHttpClientResponseFree(Resp);
    if (WriteStatus != 201 && WriteStatus != 200) {
        return EFI_DEVICE_ERROR;
    }

    F->Position += *BufferSize;
    if (F->Position > F->FileSize) {
        F->FileSize = F->Position;
    }

    // Invalidate parent dir cache and read-ahead
    CHAR8 ParentDir[MAX_PATH_LEN];
    GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
    DirCacheInvalidate(Private, ParentDir);
    F->ReadAheadLen = 0;

    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Delete
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsDelete (
    IN EFI_FILE_PROTOCOL  *This
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (Private->ReadOnly) {
        WebDavFsClose(This);
        return EFI_WARN_DELETE_FAILURE;
    }

    CHAR8 FilePath[MAX_PATH_LEN];
    AsciiSPrint(FilePath, sizeof(FilePath), "/files%a", F->Path);

    AXL_HTTP_CLIENT_RESPONSE *Resp = NULL;
    EFI_STATUS Status = WebDavFsHttpRequest(
        Private, "DELETE", FilePath, NULL, NULL, 0, &Resp);

    UINTN DelStatus = (Resp != NULL) ? Resp->StatusCode : 0;
    if (Resp != NULL) AxlHttpClientResponseFree(Resp);

    CHAR8 ParentDir[MAX_PATH_LEN];
    GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
    DirCacheInvalidate(Private, ParentDir);

    // Close (free) the file handle per spec
    WebDavFsClose(This);

    if (EFI_ERROR(Status) || (DelStatus != 200 && DelStatus != 404)) {
        return EFI_WARN_DELETE_FAILURE;
    }

    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: GetInfo / SetInfo
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsGetInfo (
    IN     EFI_FILE_PROTOCOL  *This,
    IN     EFI_GUID           *InformationType,
    IN OUT UINTN              *BufferSize,
    OUT    VOID               *Buffer
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (CompareGuid(InformationType, &gEfiFileInfoGuid)) {
        DIR_ENTRY Entry;
        SetMem(&Entry, sizeof(Entry), 0);

        if (F->IsRoot) {
            AsciiStrCpyS(Entry.Name, sizeof(Entry.Name), "");
            Entry.IsDir = TRUE;
        } else {
            // Look up from parent dir cache
            CHAR8 ParentDir[MAX_PATH_LEN];
            GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
            CONST CHAR8 *Name = GetFileName(F->Path);

            EFI_STATUS Status = DirCacheLookupEntry(Private, ParentDir, Name, &Entry);
            if (EFI_ERROR(Status)) {
                // Fallback: use what we know
                AsciiStrCpyS(Entry.Name, sizeof(Entry.Name), Name);
                Entry.Size = F->FileSize;
                Entry.IsDir = F->IsDir;
            }
        }

        UINTN Needed = BuildFileInfo(&Entry, NULL, 0);
        if (*BufferSize < Needed) {
            *BufferSize = Needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        BuildFileInfo(&Entry, (EFI_FILE_INFO *)Buffer, *BufferSize);
        *BufferSize = Needed;
        return EFI_SUCCESS;
    }

    if (CompareGuid(InformationType, &gEfiFileSystemInfoGuid)) {
        UINTN LabelLen = StrLen(VOLUME_LABEL);
        UINTN Needed = SIZE_OF_EFI_FILE_SYSTEM_INFO + (LabelLen + 1) * sizeof(CHAR16);

        if (*BufferSize < Needed) {
            *BufferSize = Needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        EFI_FILE_SYSTEM_INFO *FsInfo = (EFI_FILE_SYSTEM_INFO *)Buffer;
        SetMem(FsInfo, Needed, 0);
        FsInfo->Size = Needed;
        FsInfo->ReadOnly = Private->ReadOnly;
        FsInfo->VolumeSize = 0;
        FsInfo->FreeSpace = 0;
        FsInfo->BlockSize = 512;
        StrCpyS(FsInfo->VolumeLabel, LabelLen + 1, VOLUME_LABEL);

        *BufferSize = Needed;
        return EFI_SUCCESS;
    }

    if (CompareGuid(InformationType, &gEfiFileSystemVolumeLabelInfoIdGuid)) {
        UINTN LabelLen = StrLen(VOLUME_LABEL);
        UINTN Needed = sizeof(EFI_FILE_SYSTEM_VOLUME_LABEL) + LabelLen * sizeof(CHAR16);

        if (*BufferSize < Needed) {
            *BufferSize = Needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        EFI_FILE_SYSTEM_VOLUME_LABEL *Label = (EFI_FILE_SYSTEM_VOLUME_LABEL *)Buffer;
        StrCpyS(Label->VolumeLabel, LabelLen + 1, VOLUME_LABEL);
        *BufferSize = Needed;
        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
WebDavFsSetInfo (
    IN EFI_FILE_PROTOCOL  *This,
    IN EFI_GUID           *InformationType,
    IN UINTN              BufferSize,
    IN VOID               *Buffer
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (!CompareGuid(InformationType, &gEfiFileInfoGuid)) {
        return EFI_UNSUPPORTED;
    }

    if (Private->ReadOnly) return EFI_WRITE_PROTECTED;
    if (Buffer == NULL || BufferSize < SIZE_OF_EFI_FILE_INFO) {
        return EFI_INVALID_PARAMETER;
    }

    EFI_FILE_INFO *NewInfo = (EFI_FILE_INFO *)Buffer;

    // Check if the filename changed (rename)
    CONST CHAR8 *OldName = GetFileName(F->Path);
    CHAR8 NewName8[256];
    UINTN i = 0;
    while (NewInfo->FileName[i] != L'\0' && i < sizeof(NewName8) - 1) {
        NewName8[i] = (CHAR8)NewInfo->FileName[i];
        i++;
    }
    NewName8[i] = '\0';

    if (AsciiStrCmp(OldName, NewName8) == 0) {
        return EFI_SUCCESS;  // No name change
    }

    // Build rename request: POST /files/<oldpath>?rename=<newname>
    CHAR8 RenamePath[MAX_PATH_LEN];
    AsciiSPrint(RenamePath, sizeof(RenamePath), "/files%a?rename=%a", F->Path, NewName8);

    AXL_HTTP_CLIENT_RESPONSE *Resp = NULL;
    EFI_STATUS Status = WebDavFsHttpRequest(
        Private, "POST", RenamePath, NULL, NULL, 0, &Resp);

    UINTN RenameStatus = (Resp != NULL) ? Resp->StatusCode : 0;
    if (Resp != NULL) AxlHttpClientResponseFree(Resp);

    if (EFI_ERROR(Status) || (RenameStatus != 200 && RenameStatus != 201)) {
        return EFI_DEVICE_ERROR;
    }

    // Update internal path to reflect new name
    CHAR8 ParentDir[MAX_PATH_LEN];
    GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
    DirCacheInvalidate(Private, ParentDir);

    AsciiSPrint(F->Path, MAX_PATH_LEN, "%a%a", ParentDir, NewName8);

    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Position / Flush
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsGetPosition (
    IN  EFI_FILE_PROTOCOL  *This,
    OUT UINT64             *Position
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    if (F->IsDir) return EFI_UNSUPPORTED;
    *Position = F->Position;
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsSetPosition (
    IN EFI_FILE_PROTOCOL  *This,
    IN UINT64             Position
    )
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (F->IsDir) {
        if (Position == 0) {
            F->DirReadIndex = 0;
            return EFI_SUCCESS;
        }
        return EFI_UNSUPPORTED;
    }

    if (Position == MAX_UINT64) {
        F->Position = F->FileSize;
    } else {
        F->Position = Position;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsFlush (
    IN EFI_FILE_PROTOCOL  *This
    )
{
    return EFI_SUCCESS;
}
