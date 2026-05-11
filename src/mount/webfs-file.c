/** @file
  axl-webfs-mount -- EFI_FILE_PROTOCOL implementation.

  Every UEFI file operation translates to HTTP requests against
  xfer-server.py. Directory listings use GET /list/, file I/O uses
  GET/PUT /files/, with a 64 KB read-ahead buffer for sequential reads.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-internal.h"
#include "webfs-protocol.h"

AXL_LOG_DOMAIN("webfs-mount-file");


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

WebFsFileCtx *
webfs_create_file_handle(
    WebFsPrivate *priv,
    const char      *path,
    bool             is_dir,
    uint64_t         file_size
)
{
    WebFsFileCtx *fh = axl_calloc(1, sizeof(WebFsFileCtx));
    if (fh == NULL) return NULL;

    fh->signature = WEBFS_FILE_SIGNATURE;
    fh->private_data = priv;
    axl_strlcpy(fh->path, path, MAX_PATH_LEN);
    fh->is_dir = is_dir;
    fh->file_size = file_size;

    fh->file.Revision = EFI_FILE_PROTOCOL_REVISION;
    fh->file.Open = WebFsOpen;
    fh->file.Close = WebFsClose;
    fh->file.Delete = WebFsDelete;
    fh->file.Read = WebFsRead;
    fh->file.Write = WebFsWrite;
    fh->file.GetPosition = WebFsGetPosition;
    fh->file.SetPosition = WebFsSetPosition;
    fh->file.GetInfo = WebFsGetInfo;
    fh->file.SetInfo = WebFsSetInfo;
    fh->file.Flush = WebFsFlush;

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
WebFsOpen(
    EFI_FILE_PROTOCOL  *This,
    EFI_FILE_PROTOCOL **NewHandle,
    CHAR16             *FileName,
    UINT64              OpenMode,
    UINT64              Attributes
)
{
    WebFsFileCtx *self = WEBFS_FILE_FROM_FILE_PROTOCOL(This);
    WebFsPrivate *priv = self->private_data;

    // Handle opening "." -- return a new handle to the same path
    if (axl_wcseql(FileName, L".") || axl_wcseql(FileName, L"")) {
        WebFsFileCtx *new_fh = webfs_create_file_handle(
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

        // Create file or directory via the active protocol.
        const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
        size_t op_status = 0;
        if (Attributes & EFI_FILE_DIRECTORY) {
            if (ops->mkdir(priv, resolved, &op_status) != 0)
                return EFI_DEVICE_ERROR;
            if (op_status != 201 && op_status != 200)
                return EFI_DEVICE_ERROR;
            dir_cache_invalidate(priv, parent_dir);
            entry.is_dir = true;
            entry.size = 0;
        } else {
            if (ops->create_empty(priv, resolved, &op_status) != 0)
                return EFI_DEVICE_ERROR;
            dir_cache_invalidate(priv, parent_dir);
            entry.is_dir = false;
            entry.size = 0;
        }
    }

    // Create file handle
    WebFsFileCtx *new_fh = webfs_create_file_handle(
        priv, resolved, entry.is_dir, entry.size);
    if (new_fh == NULL) return EFI_OUT_OF_RESOURCES;

    /* Built-in integrity check (best-effort). Mark "want digest" so
       the first sequential Read can pick the `Digest:` header out of
       the Range response — no extra round-trip on Open. Once we have
       the header, digest_active flips on and reads accumulate into
       digest_ctx. Mismatch latches EFI_VOLUME_CORRUPTED on Close. */
    if (!entry.is_dir) {
        new_fh->digest_want = true;
    }

    *NewHandle = &new_fh->file;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Close
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebFsClose(
    EFI_FILE_PROTOCOL *This
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);
    EFI_STATUS    rc  = EFI_SUCCESS;

    /* Integrity verification at end-of-handle:
       - active + !failed + the whole file was read sequentially
         from 0 to file_size: compare the streaming hash to the
         expected digest the server reported on Open.
       - mismatch: surface as EFI_VOLUME_CORRUPTED. The caller (Shell
         cp, LoadImage, ...) propagates this; for cp it means the
         resulting destination is suspect and the user knows.
       - partial reads / seeks / no digest: silently skip. The
         best-effort contract is documented in webfs-internal.h. */
    if (fh->digest_active && fh->digest_ctx != NULL) {
        if (!fh->digest_failed && fh->digest_consumed == fh->file_size) {
            const char *got = axl_checksum_get_string(fh->digest_ctx);
            if (got == NULL ||
                axl_strcasecmp(got, fh->digest_expected) != 0) {
                axl_error("digest mismatch on %s: expected %s, got %s",
                          fh->path, fh->digest_expected,
                          got ? got : "(null)");
                rc = EFI_VOLUME_CORRUPTED;
            } else {
                axl_info("digest verified: %s sha-256=%s",
                         fh->path, got);
            }
        }
        axl_checksum_free(fh->digest_ctx);
        fh->digest_ctx = NULL;
    }

    if (fh->read_ahead_buf != NULL) {
        axl_free(fh->read_ahead_buf);
    }
    if (fh->dir_entries != NULL) {
        axl_free(fh->dir_entries);
    }
    axl_free(fh);
    return rc;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: Read
// ---------------------------------------------------------------------------

/// Feed exactly the bytes a successful Read returned to the user
/// into the streaming digest, but only if the position right before
/// the Read was the expected next sequential offset. Any non-
/// sequential read (post-seek, partial-then-resume) latches
/// digest_failed and disables further accumulation.
static void
digest_consume(WebFsFileCtx *fh, uint64_t pre_read_position,
               const void *bytes, size_t len)
{
    if (!fh->digest_active || fh->digest_failed) return;
    /* Sequential invariant: position before this read must equal the
       number of bytes already hashed. Track via axl_checksum's own
       state — we know that's the case iff pre_read_position equals
       the running total we've fed, which equals the consumed
       position. Simpler: validate against the file's own running
       cursor. We only get here in the read path, so position has
       been updated post-copy; pass the pre-read value in. */
    if (pre_read_position != fh->digest_consumed) {
        fh->digest_failed = true;
        return;
    }
    axl_checksum_update(fh->digest_ctx, bytes, len);
    fh->digest_consumed += len;
}

/// Read from a file (with read-ahead buffer).
static EFI_STATUS
read_file(
    WebFsFileCtx *fh,
    UINTN           *BufferSize,
    VOID            *Buffer
)
{
    WebFsPrivate *priv = fh->private_data;

    if (fh->position >= fh->file_size) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    size_t wanted = *BufferSize;
    if (fh->position + wanted > fh->file_size) {
        wanted = (size_t)(fh->file_size - fh->position);
    }
    uint64_t pre_read_position = fh->position;

    // Check read-ahead buffer
    if (fh->read_ahead_buf != NULL && fh->read_ahead_len > 0 &&
        fh->position >= fh->read_ahead_start &&
        fh->position < fh->read_ahead_start + fh->read_ahead_len) {
        size_t offset = (size_t)(fh->position - fh->read_ahead_start);
        size_t avail = fh->read_ahead_len - offset;
        size_t to_copy = (wanted < avail) ? wanted : avail;
        axl_memcpy(Buffer, fh->read_ahead_buf + offset, to_copy);
        digest_consume(fh, pre_read_position, Buffer, to_copy);
        fh->position += to_copy;
        *BufferSize = to_copy;
        return EFI_SUCCESS;
    }

    // Read-ahead miss — pull bytes via the active protocol.
    // Small reads use the 64KB read-ahead buffer; large reads
    // (LoadImage pulling a whole PE) bypass it and write directly
    // into the caller's buffer so we never under-deliver.
    bool use_readahead = (fh->read_ahead_buf != NULL &&
                          wanted < READAHEAD_BUF_SIZE);
    size_t fetch_size = use_readahead ? READAHEAD_BUF_SIZE : wanted;
    if (fh->position + fetch_size > fh->file_size) {
        fetch_size = (size_t)(fh->file_size - fh->position);
    }

    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    void *dst = use_readahead ? fh->read_ahead_buf : Buffer;
    size_t got = 0;
    /* Ask the protocol layer to capture the response's
       `Digest: sha-256=<hex>` header on this Range GET — once digest
       is in hand and we're reading sequentially from offset 0, the
       integrity check is live for the rest of the handle's life. */
    char *digest_slot = NULL;
    size_t digest_slot_size = 0;
    if (fh->digest_want && !fh->digest_active && !fh->digest_failed &&
        fh->position == 0) {
        digest_slot = fh->digest_expected;
        digest_slot_size = sizeof(fh->digest_expected);
        fh->digest_expected[0] = '\0';
    }
    if (ops->read_range(priv, fh->path, fh->position, fetch_size,
                        dst, &got,
                        digest_slot, digest_slot_size) != 0) {
        *BufferSize = 0;
        return EFI_DEVICE_ERROR;
    }
    if (digest_slot != NULL && fh->digest_expected[0] != '\0') {
        fh->digest_ctx = axl_checksum_new(AXL_CHECKSUM_SHA256);
        if (fh->digest_ctx != NULL) {
            fh->digest_active   = true;
            fh->digest_consumed = 0;
        }
        fh->digest_want = false;  // one-shot
    } else if (digest_slot != NULL) {
        /* Server didn't advertise a digest — skip validation for
           this handle. */
        fh->digest_want = false;
    }

    if (use_readahead) {
        fh->read_ahead_start = fh->position;
        fh->read_ahead_len   = got;
        size_t user_copy = (wanted < got) ? wanted : got;
        axl_memcpy(Buffer, fh->read_ahead_buf, user_copy);
        digest_consume(fh, pre_read_position, Buffer, user_copy);
        fh->position += user_copy;
        *BufferSize = user_copy;
    } else {
        digest_consume(fh, pre_read_position, Buffer, got);
        fh->position += got;
        *BufferSize = got;
        // Direct read may overlap stale read-ahead region; invalidate.
        fh->read_ahead_len = 0;
    }

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
    WebFsFileCtx *fh,
    UINTN           *BufferSize,
    VOID            *Buffer
)
{
    WebFsPrivate *priv = fh->private_data;

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
WebFsRead(
    EFI_FILE_PROTOCOL *This,
    UINTN             *BufferSize,
    VOID              *Buffer
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);

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
WebFsWrite(
    EFI_FILE_PROTOCOL *This,
    UINTN             *BufferSize,
    VOID              *Buffer
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);
    WebFsPrivate *priv = fh->private_data;

    if (fh->is_dir) return EFI_UNSUPPORTED;
    if (priv->read_only) return EFI_WRITE_PROTECTED;

    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    size_t write_status = 0;
    if (ops->write_full(priv, fh->path, Buffer, *BufferSize,
                        &write_status) != 0) {
        return EFI_DEVICE_ERROR;
    }
    if (write_status != 201 && write_status != 200 &&
        write_status != 204) {
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
WebFsDelete(
    EFI_FILE_PROTOCOL *This
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);
    WebFsPrivate *priv = fh->private_data;

    if (priv->read_only) {
        WebFsClose(This);
        return EFI_WARN_DELETE_FAILURE;
    }

    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    size_t del_status = 0;
    int ret = ops->remove(priv, fh->path, &del_status);

    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    dir_cache_invalidate(priv, parent_dir);

    // Close (free) the file handle per spec
    WebFsClose(This);

    /* 200/204 = removed cleanly; 404 = already gone (treat as
       success per the JSON path's historical behavior). 204 added
       for WebDAV servers that prefer it. */
    if (ret != 0 ||
        (del_status != 200 && del_status != 204 && del_status != 404)) {
        return EFI_WARN_DELETE_FAILURE;
    }

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL: GetInfo / SetInfo
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebFsGetInfo(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID          *InformationType,
    UINTN             *BufferSize,
    VOID              *Buffer
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);
    WebFsPrivate *priv = fh->private_data;

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
        /* The backing filesystem lives on the workstation; we don't
           ask the server how much free space it has. Reporting zero
           free space here makes UEFI Shell's cp refuse to write
           ("insufficient capacity on destination media") before it
           even attempts a single Write call. Advertise a large
           synthetic free-space value so cp proceeds and the actual
           Write/PUT exchange determines success or failure. */
        fs_info->VolumeSize = (UINT64)-1;
        fs_info->FreeSpace  = (UINT64)-1;
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
WebFsSetInfo(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID          *InformationType,
    UINTN              BufferSize,
    VOID              *Buffer
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);
    WebFsPrivate *priv = fh->private_data;

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

    // Rename via the active protocol.
    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    size_t rename_status = 0;
    int ret = ops->rename(priv, fh->path, new_name8, &rename_status);

    if (ret != 0 ||
        (rename_status != 200 && rename_status != 201 &&
         rename_status != 204)) {
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
WebFsGetPosition(
    EFI_FILE_PROTOCOL *This,
    UINT64            *Position
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);
    if (fh->is_dir) return EFI_UNSUPPORTED;
    *Position = fh->position;
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebFsSetPosition(
    EFI_FILE_PROTOCOL *This,
    UINT64             Position
)
{
    WebFsFileCtx *fh = WEBFS_FILE_FROM_FILE_PROTOCOL(This);

    if (fh->is_dir) {
        if (Position == 0) {
            fh->dir_read_index = 0;
            return EFI_SUCCESS;
        }
        return EFI_UNSUPPORTED;
    }

    /* Any seek breaks the sequential-read invariant the digest
       accumulator depends on. Latch failed; Close will skip
       validation. */
    if (fh->digest_active) {
        fh->digest_failed = true;
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
WebFsFlush(
    EFI_FILE_PROTOCOL *This
)
{
    (void)This;
    return EFI_SUCCESS;
}
