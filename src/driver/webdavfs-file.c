/** @file
  WebDavFsDxe -- EFI_FILE_PROTOCOL implementation (axl-cc port).

  Every UEFI file operation translates to HTTP requests against
  xfer-server.py. Directory listings use GET /list/, file I/O uses
  GET/PUT /files/, with a 64 KB read-ahead buffer for sequential reads.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "webdavfs-internal.h"


// ---------------------------------------------------------------------------
// Path helpers — thin wrappers around AXL SDK path functions
// ---------------------------------------------------------------------------

/// Copy dirname of Path into stack buffer Parent.
static void
GetParentPath(const char *Path, char *Parent, size_t ParentSize)
{
    char *dir = axl_path_get_dirname(Path);
    if (dir != NULL) {
        axl_strlcpy(Parent, dir, ParentSize);
        axl_free(dir);
    } else {
        axl_strlcpy(Parent, "/", ParentSize);
    }
}

/// Return basename of Path (pointer into Path or allocated — caller must NOT free).
/// For compatibility, returns pointer into the original string.
static const char *
GetFileName(const char *Path)
{
    char *base = axl_path_get_basename(Path);
    /* axl_path_get_basename allocates, but callers expect a borrowed pointer.
       Use a static buffer for simplicity (single-threaded UEFI). */
    static char sBaseName[256];
    if (base != NULL) {
        axl_strlcpy(sBaseName, base, sizeof(sBaseName));
        axl_free(base);
    } else {
        axl_strlcpy(sBaseName, Path, sizeof(sBaseName));
    }
    return sBaseName;
}

/// Resolve a CHAR16 filename relative to a base char path.
static EFI_STATUS
ResolvePath(
    const char   *BasePath,
    const CHAR16 *FileName,
    char         *Resolved,
    size_t        ResolvedSize
)
{
    // Convert CHAR16 filename to UTF-8
    char *name_utf8 = axl_ucs2_to_utf8((const unsigned short *)FileName);
    if (name_utf8 == NULL) return EFI_INVALID_PARAMETER;

    // Replace backslashes with forward slashes
    for (char *p = name_utf8; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    int rc = axl_path_resolve(BasePath, name_utf8, Resolved, ResolvedSize);
    axl_free(name_utf8);
    return (rc == 0) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

// ---------------------------------------------------------------------------
// File handle creation
// ---------------------------------------------------------------------------

WEBDAVFS_FILE *
WebDavFsCreateFileHandle(
    WEBDAVFS_PRIVATE *Private,
    const char       *Path,
    bool              IsDir,
    uint64_t          FileSize
)
{
    WEBDAVFS_FILE *F = axl_calloc(1, sizeof(WEBDAVFS_FILE));
    if (F == NULL) return NULL;

    F->Signature = WEBDAVFS_FILE_SIGNATURE;
    F->Private = Private;
    axl_strlcpy(F->Path, Path, MAX_PATH_LEN);
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
        F->ReadAheadBuf = axl_malloc(READAHEAD_BUF_SIZE);
        // NULL is OK -- we'll just skip read-ahead
    }

    return F;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Open
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsOpen(
    EFI_FILE_PROTOCOL  *This,
    EFI_FILE_PROTOCOL **NewHandle,
    CHAR16             *FileName,
    UINT64              OpenMode,
    UINT64              Attributes
)
{
    WEBDAVFS_FILE *Self = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = Self->Private;

    // Handle opening "." -- return a new handle to the same path
    if (axl_wcseql(FileName, L".") || axl_wcseql(FileName, L"")) {
        WEBDAVFS_FILE *New = WebDavFsCreateFileHandle(
            Private, Self->Path, Self->IsDir, Self->FileSize);
        if (New == NULL) return EFI_OUT_OF_RESOURCES;
        New->IsRoot = Self->IsRoot;
        *NewHandle = &New->File;
        return EFI_SUCCESS;
    }

    // Resolve the target path
    char Resolved[MAX_PATH_LEN];
    EFI_STATUS Status = ResolvePath(Self->Path, FileName, Resolved, sizeof(Resolved));
    if (EFI_ERROR(Status)) return Status;

    // Look up the entry in the parent directory
    char ParentDir[MAX_PATH_LEN];
    GetParentPath(Resolved, ParentDir, sizeof(ParentDir));
    const char *EntryName = GetFileName(Resolved);

    DirEntry Entry;
    int LookupRet = DirCacheLookupEntry(Private, ParentDir, EntryName, &Entry);

    if (LookupRet != 0) {
        // Not found -- check if CREATE mode
        if (!(OpenMode & EFI_FILE_MODE_CREATE)) {
            return EFI_NOT_FOUND;
        }

        // Create file or directory
        if (Attributes & EFI_FILE_DIRECTORY) {
            // POST /files/<path>?mkdir
            char MkdirPath[MAX_PATH_LEN];
            axl_snprintf(MkdirPath, sizeof(MkdirPath), "/files%s?mkdir", Resolved);

            AxlHttpClientResponse *Resp = NULL;
            int Ret = WebDavFsHttpRequest(
                Private, "POST", MkdirPath, NULL, NULL, 0, &Resp);
            if (Ret != 0 || Resp == NULL) return EFI_DEVICE_ERROR;

            size_t MkdirStatus = Resp->status_code;
            axl_http_client_response_free(Resp);
            if (MkdirStatus != 201 && MkdirStatus != 200) {
                return EFI_DEVICE_ERROR;
            }

            DirCacheInvalidate(Private, ParentDir);
            Entry.IsDir = true;
            Entry.Size = 0;
        } else {
            // PUT /files/<path> with empty body
            char FilePath[MAX_PATH_LEN];
            axl_snprintf(FilePath, sizeof(FilePath), "/files%s", Resolved);

            AxlHttpClientResponse *Resp = NULL;
            int Ret = WebDavFsHttpRequest(
                Private, "PUT", FilePath, NULL, "", 0, &Resp);
            if (Ret != 0 || Resp == NULL) return EFI_DEVICE_ERROR;
            axl_http_client_response_free(Resp);

            DirCacheInvalidate(Private, ParentDir);
            Entry.IsDir = false;
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

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Close
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsClose(
    EFI_FILE_PROTOCOL *This
)
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (F->ReadAheadBuf != NULL) {
        axl_free(F->ReadAheadBuf);
    }
    if (F->DirEntries != NULL && F->DirLoaded) {
        // DirEntries points into cache -- no free needed
    }
    axl_free(F);
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Read
// ---------------------------------------------------------------------------

/// Read from a file (with read-ahead buffer).
static EFI_STATUS
ReadFile(
    WEBDAVFS_FILE *F,
    UINTN         *BufferSize,
    VOID          *Buffer
)
{
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (F->Position >= F->FileSize) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    size_t Wanted = *BufferSize;
    if (F->Position + Wanted > F->FileSize) {
        Wanted = (size_t)(F->FileSize - F->Position);
    }

    // Check read-ahead buffer
    if (F->ReadAheadBuf != NULL && F->ReadAheadLen > 0 &&
        F->Position >= F->ReadAheadStart &&
        F->Position < F->ReadAheadStart + F->ReadAheadLen) {
        size_t Offset = (size_t)(F->Position - F->ReadAheadStart);
        size_t Available = F->ReadAheadLen - Offset;
        size_t ToCopy = (Wanted < Available) ? Wanted : Available;
        axl_memcpy(Buffer, F->ReadAheadBuf + Offset, ToCopy);
        F->Position += ToCopy;
        *BufferSize = ToCopy;
        return EFI_SUCCESS;
    }

    // Read-ahead miss -- fetch from server with read-ahead
    size_t FetchSize = Wanted;
    if (F->ReadAheadBuf != NULL && FetchSize < READAHEAD_BUF_SIZE) {
        FetchSize = READAHEAD_BUF_SIZE;
    }
    if (F->Position + FetchSize > F->FileSize) {
        FetchSize = (size_t)(F->FileSize - F->Position);
    }

    // Build Range header
    char RangeVal[64];
    axl_snprintf(RangeVal, sizeof(RangeVal), "bytes=%llu-%llu",
                 (unsigned long long)F->Position,
                 (unsigned long long)(F->Position + FetchSize - 1));
    AxlHashTable *RangeHdrs = axl_hash_table_new();
    if (RangeHdrs == NULL) return EFI_OUT_OF_RESOURCES;
    axl_hash_table_set(RangeHdrs, "range", axl_strdup(RangeVal));

    char FilePath[MAX_PATH_LEN];
    axl_snprintf(FilePath, sizeof(FilePath), "/files%s", F->Path);

    AxlHttpClientResponse *Resp = NULL;
    int Ret = WebDavFsHttpRequest(
        Private, "GET", FilePath, RangeHdrs, NULL, 0, &Resp);
    axl_free(axl_hash_table_get(RangeHdrs, "range"));
    axl_hash_table_free(RangeHdrs);
    if (Ret != 0 || Resp == NULL) return EFI_DEVICE_ERROR;
    if (Resp->status_code != 200 && Resp->status_code != 206) {
        axl_http_client_response_free(Resp);
        *BufferSize = 0;
        return EFI_DEVICE_ERROR;
    }

    // Copy response body into read-ahead buffer or directly into caller's buffer
    size_t TotalRead = Resp->body_size;
    if (TotalRead > FetchSize) TotalRead = FetchSize;

    if (F->ReadAheadBuf != NULL) {
        size_t ToCopy = (TotalRead < READAHEAD_BUF_SIZE) ? TotalRead : READAHEAD_BUF_SIZE;
        axl_memcpy(F->ReadAheadBuf, Resp->body, ToCopy);
        F->ReadAheadStart = F->Position;
        F->ReadAheadLen = ToCopy;
        size_t UserCopy = (Wanted < ToCopy) ? Wanted : ToCopy;
        axl_memcpy(Buffer, F->ReadAheadBuf, UserCopy);
        F->Position += UserCopy;
        *BufferSize = UserCopy;
    } else {
        size_t ToCopy = (Wanted < TotalRead) ? Wanted : TotalRead;
        axl_memcpy(Buffer, Resp->body, ToCopy);
        F->Position += ToCopy;
        *BufferSize = ToCopy;
    }
    axl_http_client_response_free(Resp);

    return EFI_SUCCESS;
}

/// Build an EFI_FILE_INFO from a DirEntry, converting name to UCS-2.
static size_t
BuildFileInfo(
    DirEntry       *Entry,
    EFI_FILE_INFO  *Info,
    size_t          BufSize
)
{
    // Calculate needed size
    size_t NameLen = axl_strlen(Entry->Name);
    size_t NeededSize = SIZE_OF_EFI_FILE_INFO + (NameLen + 1) * sizeof(CHAR16);

    if (Info == NULL || BufSize < NeededSize) return NeededSize;

    axl_memset(Info, 0, NeededSize);
    Info->Size = NeededSize;
    Info->FileSize = Entry->Size;
    Info->PhysicalSize = Entry->Size;
    if (Entry->IsDir) {
        Info->Attribute = EFI_FILE_DIRECTORY;
    }

    // Convert name to UCS-2
    for (size_t i = 0; i <= NameLen; i++) {
        Info->FileName[i] = (CHAR16)(unsigned char)Entry->Name[i];
    }

    return NeededSize;
}

/// Read next directory entry.
static EFI_STATUS
ReadDir(
    WEBDAVFS_FILE *F,
    UINTN         *BufferSize,
    VOID          *Buffer
)
{
    WEBDAVFS_PRIVATE *Private = F->Private;

    // Lazy-load directory listing
    if (!F->DirLoaded) {
        int Ret = DirCacheFetch(
            Private, F->Path, &F->DirEntries, &F->DirEntryCount);
        if (Ret != 0) return EFI_DEVICE_ERROR;
        F->DirLoaded = true;
        F->DirReadIndex = 0;
    }

    // End of directory
    if (F->DirReadIndex >= F->DirEntryCount) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    DirEntry *Entry = &F->DirEntries[F->DirReadIndex];
    size_t Needed = BuildFileInfo(Entry, NULL, 0);

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
WebDavFsRead(
    EFI_FILE_PROTOCOL *This,
    UINTN             *BufferSize,
    VOID              *Buffer
)
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (F->IsDir) {
        return ReadDir(F, BufferSize, Buffer);
    } else {
        return ReadFile(F, BufferSize, Buffer);
    }
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Write
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsWrite(
    EFI_FILE_PROTOCOL *This,
    UINTN             *BufferSize,
    VOID              *Buffer
)
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (F->IsDir) return EFI_UNSUPPORTED;
    if (Private->ReadOnly) return EFI_WRITE_PROTECTED;

    char FilePath[MAX_PATH_LEN];
    axl_snprintf(FilePath, sizeof(FilePath), "/files%s", F->Path);

    AxlHttpClientResponse *Resp = NULL;
    int Ret = WebDavFsHttpRequest(
        Private, "PUT", FilePath, NULL, Buffer, *BufferSize, &Resp);
    if (Ret != 0 || Resp == NULL) return EFI_DEVICE_ERROR;

    size_t WriteStatus = Resp->status_code;
    axl_http_client_response_free(Resp);
    if (WriteStatus != 201 && WriteStatus != 200) {
        return EFI_DEVICE_ERROR;
    }

    F->Position += *BufferSize;
    if (F->Position > F->FileSize) {
        F->FileSize = F->Position;
    }

    // Invalidate parent dir cache and read-ahead
    char ParentDir[MAX_PATH_LEN];
    GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
    DirCacheInvalidate(Private, ParentDir);
    F->ReadAheadLen = 0;

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Delete
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsDelete(
    EFI_FILE_PROTOCOL *This
)
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (Private->ReadOnly) {
        WebDavFsClose(This);
        return EFI_WARN_DELETE_FAILURE;
    }

    char FilePath[MAX_PATH_LEN];
    axl_snprintf(FilePath, sizeof(FilePath), "/files%s", F->Path);

    AxlHttpClientResponse *Resp = NULL;
    int Ret = WebDavFsHttpRequest(
        Private, "DELETE", FilePath, NULL, NULL, 0, &Resp);

    size_t DelStatus = (Resp != NULL) ? Resp->status_code : 0;
    if (Resp != NULL) axl_http_client_response_free(Resp);

    char ParentDir[MAX_PATH_LEN];
    GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
    DirCacheInvalidate(Private, ParentDir);

    // Close (free) the file handle per spec
    WebDavFsClose(This);

    if (Ret != 0 || (DelStatus != 200 && DelStatus != 404)) {
        return EFI_WARN_DELETE_FAILURE;
    }

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: GetInfo / SetInfo
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsGetInfo(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID          *InformationType,
    UINTN             *BufferSize,
    VOID              *Buffer
)
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (axl_guid_equal(InformationType, &gEfiFileInfoGuid)) {
        DirEntry Entry;
        axl_memset(&Entry, 0, sizeof(Entry));

        if (F->IsRoot) {
            axl_strlcpy(Entry.Name, "", sizeof(Entry.Name));
            Entry.IsDir = true;
        } else {
            // Look up from parent dir cache
            char ParentDir[MAX_PATH_LEN];
            GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
            const char *Name = GetFileName(F->Path);

            int Ret = DirCacheLookupEntry(Private, ParentDir, Name, &Entry);
            if (Ret != 0) {
                // Fallback: use what we know
                axl_strlcpy(Entry.Name, Name, sizeof(Entry.Name));
                Entry.Size = F->FileSize;
                Entry.IsDir = F->IsDir;
            }
        }

        size_t Needed = BuildFileInfo(&Entry, NULL, 0);
        if (*BufferSize < Needed) {
            *BufferSize = Needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        BuildFileInfo(&Entry, (EFI_FILE_INFO *)Buffer, *BufferSize);
        *BufferSize = Needed;
        return EFI_SUCCESS;
    }

    if (axl_guid_equal(InformationType, &gEfiFileSystemInfoGuid)) {
        size_t LabelLen = axl_wcslen(VOLUME_LABEL);
        size_t Needed = SIZE_OF_EFI_FILE_SYSTEM_INFO + (LabelLen + 1) * sizeof(CHAR16);

        if (*BufferSize < Needed) {
            *BufferSize = Needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        EFI_FILE_SYSTEM_INFO *FsInfo = (EFI_FILE_SYSTEM_INFO *)Buffer;
        axl_memset(FsInfo, 0, Needed);
        FsInfo->Size = Needed;
        FsInfo->ReadOnly = Private->ReadOnly;
        FsInfo->VolumeSize = 0;
        FsInfo->FreeSpace = 0;
        FsInfo->BlockSize = 512;
        axl_wcscpy(FsInfo->VolumeLabel, VOLUME_LABEL, LabelLen + 1);

        *BufferSize = Needed;
        return EFI_SUCCESS;
    }

    if (axl_guid_equal(InformationType, &gEfiFileSystemVolumeLabelInfoIdGuid)) {
        size_t LabelLen = axl_wcslen(VOLUME_LABEL);
        size_t Needed = sizeof(EFI_FILE_SYSTEM_VOLUME_LABEL) + (LabelLen + 1) * sizeof(CHAR16);

        if (*BufferSize < Needed) {
            *BufferSize = Needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        EFI_FILE_SYSTEM_VOLUME_LABEL *Label = (EFI_FILE_SYSTEM_VOLUME_LABEL *)Buffer;
        axl_wcscpy(Label->VolumeLabel, VOLUME_LABEL, LabelLen + 1);
        *BufferSize = Needed;
        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
WebDavFsSetInfo(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID          *InformationType,
    UINTN              BufferSize,
    VOID              *Buffer
)
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WEBDAVFS_PRIVATE *Private = F->Private;

    if (!axl_guid_equal(InformationType, &gEfiFileInfoGuid)) {
        return EFI_UNSUPPORTED;
    }

    if (Private->ReadOnly) return EFI_WRITE_PROTECTED;
    if (Buffer == NULL || BufferSize < SIZE_OF_EFI_FILE_INFO) {
        return EFI_INVALID_PARAMETER;
    }

    EFI_FILE_INFO *NewInfo = (EFI_FILE_INFO *)Buffer;

    // Check if the filename changed (rename)
    const char *OldName = GetFileName(F->Path);
    char NewName8[256];
    size_t i = 0;
    while (NewInfo->FileName[i] != L'\0' && i < sizeof(NewName8) - 1) {
        NewName8[i] = (char)NewInfo->FileName[i];
        i++;
    }
    NewName8[i] = '\0';

    if (axl_streql(OldName, NewName8)) {
        return EFI_SUCCESS;  // No name change
    }

    // Build rename request: POST /files/<oldpath>?rename=<newname>
    char RenamePath[MAX_PATH_LEN];
    axl_snprintf(RenamePath, sizeof(RenamePath), "/files%s?rename=%s", F->Path, NewName8);

    AxlHttpClientResponse *Resp = NULL;
    int Ret = WebDavFsHttpRequest(
        Private, "POST", RenamePath, NULL, NULL, 0, &Resp);

    size_t RenameStatus = (Resp != NULL) ? Resp->status_code : 0;
    if (Resp != NULL) axl_http_client_response_free(Resp);

    if (Ret != 0 || (RenameStatus != 200 && RenameStatus != 201)) {
        return EFI_DEVICE_ERROR;
    }

    // Update internal path to reflect new name
    char ParentDir[MAX_PATH_LEN];
    GetParentPath(F->Path, ParentDir, sizeof(ParentDir));
    DirCacheInvalidate(Private, ParentDir);

    axl_snprintf(F->Path, MAX_PATH_LEN, "%s%s", ParentDir, NewName8);

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Position / Flush
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsGetPosition(
    EFI_FILE_PROTOCOL *This,
    UINT64            *Position
)
{
    WEBDAVFS_FILE *F = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    if (F->IsDir) return EFI_UNSUPPORTED;
    *Position = F->Position;
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsSetPosition(
    EFI_FILE_PROTOCOL *This,
    UINT64             Position
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

    if (Position == UINT64_MAX) {
        F->Position = F->FileSize;
    } else {
        F->Position = Position;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsFlush(
    EFI_FILE_PROTOCOL *This
)
{
    (void)This;
    return EFI_SUCCESS;
}
