/** @file
  WebDavFsDxe -- EFI_FILE_PROTOCOL implementation.

  Every UEFI file operation translates to HTTP requests against
  xfer-server.py. Directory listings use GET /list/, file I/O uses
  GET/PUT /files/, with a 64 KB read-ahead buffer for sequential reads.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "webdavfs-internal.h"


// ---------------------------------------------------------------------------
// Path helpers -- thin wrappers around AXL SDK path functions
// ---------------------------------------------------------------------------

/// Copy dirname of path into stack buffer parent.
static void
get_parent_path(const char *path, char *parent, size_t parent_size)
{
    char *dir = axl_path_get_dirname(path);
    if (dir != NULL) {
        axl_strlcpy(parent, dir, parent_size);
        axl_free(dir);
    } else {
        axl_strlcpy(parent, "/", parent_size);
    }
}

/// Return basename of path (pointer into static buffer -- single-threaded UEFI).
static const char *
get_file_name(const char *path)
{
    char *base = axl_path_get_basename(path);
    /* axl_path_get_basename allocates, but callers expect a borrowed pointer.
       Use a static buffer for simplicity (single-threaded UEFI). */
    static char s_base_name[256];
    if (base != NULL) {
        axl_strlcpy(s_base_name, base, sizeof(s_base_name));
        axl_free(base);
    } else {
        axl_strlcpy(s_base_name, path, sizeof(s_base_name));
    }
    return s_base_name;
}

/// Resolve a CHAR16 filename relative to a base char path.
static int
resolve_path(
    const char           *base_path,
    const unsigned short *file_name,
    char                 *resolved,
    size_t                resolved_size
)
{
    // Convert CHAR16 filename to UTF-8
    char *name_utf8 = axl_ucs2_to_utf8(file_name);
    if (name_utf8 == NULL) return -1;

    // Replace backslashes with forward slashes
    for (char *p = name_utf8; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    int rc = axl_path_resolve(base_path, name_utf8, resolved, resolved_size);
    axl_free(name_utf8);
    return rc;
}

// ---------------------------------------------------------------------------
// File handle creation
// ---------------------------------------------------------------------------

WebDavFsFileCtx *
webdavfs_create_file_handle(
    WebDavFsPrivate *priv,
    const char      *path,
    bool             is_dir,
    uint64_t         file_size
)
{
    WebDavFsFileCtx *fh = axl_calloc(1, sizeof(WebDavFsFileCtx));
    if (fh == NULL) return NULL;

    fh->signature = WEBDAVFS_FILE_SIGNATURE;
    fh->private_data = priv;
    axl_strlcpy(fh->path, path, MAX_PATH_LEN);
    fh->is_dir = is_dir;
    fh->file_size = file_size;

    fh->file.Revision = EFI_FILE_PROTOCOL_REVISION;
    fh->file.Open = WebDavFsOpen;
    fh->file.Close = WebDavFsClose;
    fh->file.Delete = WebDavFsDelete;
    fh->file.Read = WebDavFsRead;
    fh->file.Write = WebDavFsWrite;
    fh->file.GetPosition = WebDavFsGetPosition;
    fh->file.SetPosition = WebDavFsSetPosition;
    fh->file.GetInfo = WebDavFsGetInfo;
    fh->file.SetInfo = WebDavFsSetInfo;
    fh->file.Flush = WebDavFsFlush;

    // Allocate read-ahead buffer for files
    if (!is_dir) {
        fh->read_ahead_buf = axl_malloc(READAHEAD_BUF_SIZE);
        // NULL is OK -- we'll just skip read-ahead
    }

    return fh;
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
    WebDavFsFileCtx *self = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WebDavFsPrivate *priv = self->private_data;

    // Handle opening "." -- return a new handle to the same path
    if (axl_wcseql(FileName, L".") || axl_wcseql(FileName, L"")) {
        WebDavFsFileCtx *new_fh = webdavfs_create_file_handle(
            priv, self->path, self->is_dir, self->file_size);
        if (new_fh == NULL) return EFI_OUT_OF_RESOURCES;
        new_fh->is_root = self->is_root;
        *NewHandle = &new_fh->file;
        return EFI_SUCCESS;
    }

    // Resolve the target path
    char resolved[MAX_PATH_LEN];
    int status = resolve_path(self->path, (const unsigned short *)FileName,
                              resolved, sizeof(resolved));
    if (status != 0) return EFI_INVALID_PARAMETER;

    // Look up the entry in the parent directory
    char parent_dir[MAX_PATH_LEN];
    get_parent_path(resolved, parent_dir, sizeof(parent_dir));
    const char *entry_name = get_file_name(resolved);

    DirEntry entry;
    int lookup_ret = dir_cache_lookup_entry(priv, parent_dir, entry_name, &entry);

    if (lookup_ret != 0) {
        // Not found -- check if CREATE mode
        if (!(OpenMode & EFI_FILE_MODE_CREATE)) {
            return EFI_NOT_FOUND;
        }

        // Create file or directory
        if (Attributes & EFI_FILE_DIRECTORY) {
            // POST /files/<path>?mkdir
            char mkdir_path[MAX_PATH_LEN];
            axl_snprintf(mkdir_path, sizeof(mkdir_path), "/files%s?mkdir", resolved);

            AxlHttpClientResponse *resp = NULL;
            int ret = webdavfs_http_request(
                priv, "POST", mkdir_path, NULL, NULL, 0, &resp);
            if (ret != 0 || resp == NULL) return EFI_DEVICE_ERROR;

            size_t mkdir_status = resp->status_code;
            axl_http_client_response_free(resp);
            if (mkdir_status != 201 && mkdir_status != 200) {
                return EFI_DEVICE_ERROR;
            }

            dir_cache_invalidate(priv, parent_dir);
            entry.is_dir = true;
            entry.size = 0;
        } else {
            // PUT /files/<path> with empty body
            char file_path[MAX_PATH_LEN];
            axl_snprintf(file_path, sizeof(file_path), "/files%s", resolved);

            AxlHttpClientResponse *resp = NULL;
            int ret = webdavfs_http_request(
                priv, "PUT", file_path, NULL, "", 0, &resp);
            if (ret != 0 || resp == NULL) return EFI_DEVICE_ERROR;
            axl_http_client_response_free(resp);

            dir_cache_invalidate(priv, parent_dir);
            entry.is_dir = false;
            entry.size = 0;
        }
    }

    // Create file handle
    WebDavFsFileCtx *new_fh = webdavfs_create_file_handle(
        priv, resolved, entry.is_dir, entry.size);
    if (new_fh == NULL) return EFI_OUT_OF_RESOURCES;

    *NewHandle = &new_fh->file;
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
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (fh->read_ahead_buf != NULL) {
        axl_free(fh->read_ahead_buf);
    }
    if (fh->dir_entries != NULL) {
        axl_free(fh->dir_entries);
    }
    axl_free(fh);
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Read
// ---------------------------------------------------------------------------

/// Read from a file (with read-ahead buffer).
static EFI_STATUS
read_file(
    WebDavFsFileCtx *fh,
    UINTN           *BufferSize,
    VOID            *Buffer
)
{
    WebDavFsPrivate *priv = fh->private_data;

    if (fh->position >= fh->file_size) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    size_t wanted = *BufferSize;
    if (fh->position + wanted > fh->file_size) {
        wanted = (size_t)(fh->file_size - fh->position);
    }

    // Check read-ahead buffer
    if (fh->read_ahead_buf != NULL && fh->read_ahead_len > 0 &&
        fh->position >= fh->read_ahead_start &&
        fh->position < fh->read_ahead_start + fh->read_ahead_len) {
        size_t offset = (size_t)(fh->position - fh->read_ahead_start);
        size_t avail = fh->read_ahead_len - offset;
        size_t to_copy = (wanted < avail) ? wanted : avail;
        axl_memcpy(Buffer, fh->read_ahead_buf + offset, to_copy);
        fh->position += to_copy;
        *BufferSize = to_copy;
        return EFI_SUCCESS;
    }

    // Read-ahead miss -- fetch from server with read-ahead
    size_t fetch_size = wanted;
    if (fh->read_ahead_buf != NULL && fetch_size < READAHEAD_BUF_SIZE) {
        fetch_size = READAHEAD_BUF_SIZE;
    }
    if (fh->position + fetch_size > fh->file_size) {
        fetch_size = (size_t)(fh->file_size - fh->position);
    }

    // Build Range header
    char range_val[64];
    axl_snprintf(range_val, sizeof(range_val), "bytes=%llu-%llu",
                 (unsigned long long)fh->position,
                 (unsigned long long)(fh->position + fetch_size - 1));
    AxlHashTable *range_hdrs = axl_hash_table_new();
    if (range_hdrs == NULL) return EFI_OUT_OF_RESOURCES;
    axl_hash_table_insert(range_hdrs, "range", axl_strdup(range_val));

    char file_path[MAX_PATH_LEN];
    axl_snprintf(file_path, sizeof(file_path), "/files%s", fh->path);

    AxlHttpClientResponse *resp = NULL;
    int ret = webdavfs_http_request(
        priv, "GET", file_path, range_hdrs, NULL, 0, &resp);
    axl_free(axl_hash_table_lookup(range_hdrs, "range"));
    axl_hash_table_free(range_hdrs);
    if (ret != 0 || resp == NULL) return EFI_DEVICE_ERROR;
    if (resp->status_code != 200 && resp->status_code != 206) {
        axl_http_client_response_free(resp);
        *BufferSize = 0;
        return EFI_DEVICE_ERROR;
    }

    // Copy response body into read-ahead buffer or directly into caller's buffer
    size_t total_read = resp->body_size;
    if (total_read > fetch_size) total_read = fetch_size;

    if (fh->read_ahead_buf != NULL) {
        size_t to_copy = (total_read < READAHEAD_BUF_SIZE) ? total_read : READAHEAD_BUF_SIZE;
        axl_memcpy(fh->read_ahead_buf, resp->body, to_copy);
        fh->read_ahead_start = fh->position;
        fh->read_ahead_len = to_copy;
        size_t user_copy = (wanted < to_copy) ? wanted : to_copy;
        axl_memcpy(Buffer, fh->read_ahead_buf, user_copy);
        fh->position += user_copy;
        *BufferSize = user_copy;
    } else {
        size_t to_copy = (wanted < total_read) ? wanted : total_read;
        axl_memcpy(Buffer, resp->body, to_copy);
        fh->position += to_copy;
        *BufferSize = to_copy;
    }
    axl_http_client_response_free(resp);

    return EFI_SUCCESS;
}

/// Build an EFI_FILE_INFO from a DirEntry, converting name to UCS-2.
static size_t
build_file_info(
    DirEntry       *entry,
    EFI_FILE_INFO  *Info,
    size_t          buf_size
)
{
    // Calculate needed size
    size_t name_len = axl_strlen(entry->name);
    size_t needed = SIZE_OF_EFI_FILE_INFO + (name_len + 1) * sizeof(CHAR16);

    if (Info == NULL || buf_size < needed) return needed;

    axl_memset(Info, 0, needed);
    Info->Size = needed;
    Info->FileSize = entry->size;
    Info->PhysicalSize = entry->size;
    if (entry->is_dir) {
        Info->Attribute = EFI_FILE_DIRECTORY;
    }

    // Convert name to UCS-2
    for (size_t i = 0; i <= name_len; i++) {
        Info->FileName[i] = (CHAR16)(unsigned char)entry->name[i];
    }

    return needed;
}

/// Read next directory entry.
static EFI_STATUS
read_dir(
    WebDavFsFileCtx *fh,
    UINTN           *BufferSize,
    VOID            *Buffer
)
{
    WebDavFsPrivate *priv = fh->private_data;

    // Lazy-load directory listing (copy from cache for stability)
    if (!fh->dir_loaded) {
        DirEntry *cache_entries = NULL;
        size_t cache_count = 0;
        int ret = dir_cache_fetch(
            priv, fh->path, &cache_entries, &cache_count);
        if (ret != 0) return EFI_DEVICE_ERROR;

        fh->dir_entries = axl_calloc(cache_count, sizeof(DirEntry));
        if (fh->dir_entries == NULL) return EFI_OUT_OF_RESOURCES;
        axl_memcpy(fh->dir_entries, cache_entries, cache_count * sizeof(DirEntry));
        fh->dir_entry_count = cache_count;
        fh->dir_loaded = true;
        fh->dir_read_index = 0;
    }

    // End of directory
    if (fh->dir_read_index >= fh->dir_entry_count) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    DirEntry *entry = &fh->dir_entries[fh->dir_read_index];
    size_t needed = build_file_info(entry, NULL, 0);

    if (*BufferSize < needed) {
        *BufferSize = needed;
        return EFI_BUFFER_TOO_SMALL;
    }

    build_file_info(entry, (EFI_FILE_INFO *)Buffer, *BufferSize);
    *BufferSize = needed;
    fh->dir_read_index++;
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
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (fh->is_dir) {
        return read_dir(fh, BufferSize, Buffer);
    } else {
        return read_file(fh, BufferSize, Buffer);
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
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WebDavFsPrivate *priv = fh->private_data;

    if (fh->is_dir) return EFI_UNSUPPORTED;
    if (priv->read_only) return EFI_WRITE_PROTECTED;

    char file_path[MAX_PATH_LEN];
    axl_snprintf(file_path, sizeof(file_path), "/files%s", fh->path);

    AxlHttpClientResponse *resp = NULL;
    int ret = webdavfs_http_request(
        priv, "PUT", file_path, NULL, Buffer, *BufferSize, &resp);
    if (ret != 0 || resp == NULL) return EFI_DEVICE_ERROR;

    size_t write_status = resp->status_code;
    axl_http_client_response_free(resp);
    if (write_status != 201 && write_status != 200) {
        return EFI_DEVICE_ERROR;
    }

    fh->position += *BufferSize;
    if (fh->position > fh->file_size) {
        fh->file_size = fh->position;
    }

    // Invalidate parent dir cache and read-ahead
    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    dir_cache_invalidate(priv, parent_dir);
    fh->read_ahead_len = 0;

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
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WebDavFsPrivate *priv = fh->private_data;

    if (priv->read_only) {
        WebDavFsClose(This);
        return EFI_WARN_DELETE_FAILURE;
    }

    char file_path[MAX_PATH_LEN];
    axl_snprintf(file_path, sizeof(file_path), "/files%s", fh->path);

    AxlHttpClientResponse *resp = NULL;
    int ret = webdavfs_http_request(
        priv, "DELETE", file_path, NULL, NULL, 0, &resp);

    size_t del_status = (resp != NULL) ? resp->status_code : 0;
    if (resp != NULL) axl_http_client_response_free(resp);

    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    dir_cache_invalidate(priv, parent_dir);

    // Close (free) the file handle per spec
    WebDavFsClose(This);

    if (ret != 0 || (del_status != 200 && del_status != 404)) {
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
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WebDavFsPrivate *priv = fh->private_data;

    if (axl_guid_equal(InformationType, &gEfiFileInfoGuid)) {
        DirEntry entry;
        axl_memset(&entry, 0, sizeof(entry));

        if (fh->is_root) {
            axl_strlcpy(entry.name, "", sizeof(entry.name));
            entry.is_dir = true;
        } else {
            // Look up from parent dir cache
            char parent_dir[MAX_PATH_LEN];
            get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
            const char *name = get_file_name(fh->path);

            int ret = dir_cache_lookup_entry(priv, parent_dir, name, &entry);
            if (ret != 0) {
                // Fallback: use what we know
                axl_strlcpy(entry.name, name, sizeof(entry.name));
                entry.size = fh->file_size;
                entry.is_dir = fh->is_dir;
            }
        }

        size_t needed = build_file_info(&entry, NULL, 0);
        if (*BufferSize < needed) {
            *BufferSize = needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        build_file_info(&entry, (EFI_FILE_INFO *)Buffer, *BufferSize);
        *BufferSize = needed;
        return EFI_SUCCESS;
    }

    if (axl_guid_equal(InformationType, &gEfiFileSystemInfoGuid)) {
        size_t label_len = axl_wcslen(VOLUME_LABEL);
        size_t needed = SIZE_OF_EFI_FILE_SYSTEM_INFO + (label_len + 1) * sizeof(CHAR16);

        if (*BufferSize < needed) {
            *BufferSize = needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        EFI_FILE_SYSTEM_INFO *fs_info = (EFI_FILE_SYSTEM_INFO *)Buffer;
        axl_memset(fs_info, 0, needed);
        fs_info->Size = needed;
        fs_info->ReadOnly = priv->read_only;
        fs_info->VolumeSize = 0;
        fs_info->FreeSpace = 0;
        fs_info->BlockSize = 512;
        axl_wcscpy(fs_info->VolumeLabel, VOLUME_LABEL, label_len + 1);

        *BufferSize = needed;
        return EFI_SUCCESS;
    }

    if (axl_guid_equal(InformationType, &gEfiFileSystemVolumeLabelInfoIdGuid)) {
        size_t label_len = axl_wcslen(VOLUME_LABEL);
        size_t needed = sizeof(EFI_FILE_SYSTEM_VOLUME_LABEL) + (label_len + 1) * sizeof(CHAR16);

        if (*BufferSize < needed) {
            *BufferSize = needed;
            return EFI_BUFFER_TOO_SMALL;
        }

        EFI_FILE_SYSTEM_VOLUME_LABEL *label = (EFI_FILE_SYSTEM_VOLUME_LABEL *)Buffer;
        axl_wcscpy(label->VolumeLabel, VOLUME_LABEL, label_len + 1);
        *BufferSize = needed;
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
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    WebDavFsPrivate *priv = fh->private_data;

    if (!axl_guid_equal(InformationType, &gEfiFileInfoGuid)) {
        return EFI_UNSUPPORTED;
    }

    if (priv->read_only) return EFI_WRITE_PROTECTED;
    if (Buffer == NULL || BufferSize < SIZE_OF_EFI_FILE_INFO) {
        return EFI_INVALID_PARAMETER;
    }

    EFI_FILE_INFO *new_info = (EFI_FILE_INFO *)Buffer;

    // Check if the filename changed (rename)
    const char *old_name = get_file_name(fh->path);
    char new_name8[256];
    size_t i = 0;
    while (new_info->FileName[i] != L'\0' && i < sizeof(new_name8) - 1) {
        new_name8[i] = (char)new_info->FileName[i];
        i++;
    }
    new_name8[i] = '\0';

    if (axl_streql(old_name, new_name8)) {
        return EFI_SUCCESS;  // No name change
    }

    // Build rename request: POST /files/<oldpath>?rename=<newname>
    char rename_path[MAX_PATH_LEN];
    axl_snprintf(rename_path, sizeof(rename_path), "/files%s?rename=%s", fh->path, new_name8);

    AxlHttpClientResponse *resp = NULL;
    int ret = webdavfs_http_request(
        priv, "POST", rename_path, NULL, NULL, 0, &resp);

    size_t rename_status = (resp != NULL) ? resp->status_code : 0;
    if (resp != NULL) axl_http_client_response_free(resp);

    if (ret != 0 || (rename_status != 200 && rename_status != 201)) {
        return EFI_DEVICE_ERROR;
    }

    // Update internal path to reflect new name
    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    dir_cache_invalidate(priv, parent_dir);

    axl_snprintf(fh->path, MAX_PATH_LEN, "%s%s", parent_dir, new_name8);

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
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);
    if (fh->is_dir) return EFI_UNSUPPORTED;
    *Position = fh->position;
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsSetPosition(
    EFI_FILE_PROTOCOL *This,
    UINT64             Position
)
{
    WebDavFsFileCtx *fh = WEBDAVFS_FILE_FROM_FILE_PROTOCOL(This);

    if (fh->is_dir) {
        if (Position == 0) {
            fh->dir_read_index = 0;
            return EFI_SUCCESS;
        }
        return EFI_UNSUPPORTED;
    }

    if (Position == UINT64_MAX) {
        fh->position = fh->file_size;
    } else {
        fh->position = Position;
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
