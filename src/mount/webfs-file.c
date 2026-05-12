/** @file
  axl-webfs-mount -- AxlFsProvider callback implementation.

  Speaks pure UTF-8 + AxlFsStatus. Every call translates to one or
  more HTTP requests against xfer-server.py via webfs-protocol's
  vtable; the SDK thunks marshal CHAR16 ↔ UTF-8, EFI_FILE_INFO,
  and EFI_STATUS for UEFI consumers (Shell, LoadImage, Boot
  Manager) at the boundary in <axl/axl-fs-provider.h>.

  Pre-Phase-C version of this file synthesized 12 EFIAPI thunks
  inline and hand-rolled the EFI_FILE_INFO trailer + UCS-2
  conversion. All of that lives in axl-sdk now.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-internal.h"
#include "webfs-protocol.h"

AXL_LOG_DOMAIN("webfs-mount-file");

/* Forward decl — webfs_close drains buffered Writes via this helper
   that lives down by webfs_write where the buffer state is touched. */
static AxlFsStatus flush_put_buf(AxlFsProviderFile *fh);

// ---------------------------------------------------------------------------
// Path helpers — thin wrappers around AXL SDK path primitives.
// ---------------------------------------------------------------------------

/// Copy dirname of @p path into @p parent.
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

/// Return basename of @p path (pointer into a single-threaded
/// static buffer — fine for UEFI).
static const char *
get_file_name(const char *path)
{
    char *base = axl_path_get_basename(path);
    static char s_base_name[256];
    if (base != NULL) {
        axl_strlcpy(s_base_name, base, sizeof(s_base_name));
        axl_free(base);
    } else {
        axl_strlcpy(s_base_name, path, sizeof(s_base_name));
    }
    return s_base_name;
}

// ---------------------------------------------------------------------------
// File-handle allocation
// ---------------------------------------------------------------------------

static AxlFsProviderFile *
webfs_alloc_file(WebFsPrivate *priv, const char *path,
                 bool is_dir, uint64_t file_size)
{
    AxlFsProviderFile *fh = axl_calloc(1, sizeof(*fh));
    if (fh == NULL) return NULL;
    fh->priv = priv;
    axl_strlcpy(fh->path, path, sizeof(fh->path));
    fh->is_dir    = is_dir;
    fh->file_size = file_size;

    if (!is_dir) {
        fh->read_ahead_buf = axl_malloc(READAHEAD_BUF_SIZE);
        /* NULL is fine — we just skip read-ahead. */
    }
    return fh;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_open(
    void               *backend_ctx,
    const char         *path,
    unsigned            mode,
    unsigned            attributes,
    AxlFsProviderFile **out,
    bool               *out_is_dir
    )
{
    WebFsPrivate *priv = backend_ctx;
    if (path == NULL || out == NULL || out_is_dir == NULL) {
        return AXL_FS_ERR_INVALID;
    }

    /* Root open. The SDK calls us with "/" from OpenVolume; child
       opens come pre-resolved by the SDK against the parent's
       cached path, so we always see an absolute UTF-8 path with
       '/' separators. */
    bool is_root = (path[0] == '/' && path[1] == '\0');
    if (is_root) {
        AxlFsProviderFile *fh = webfs_alloc_file(priv, "/", true, 0);
        if (fh == NULL) return AXL_FS_ERR_NO_MEMORY;
        fh->is_root = true;
        *out = fh;
        *out_is_dir = true;
        return AXL_FS_OK;
    }

    /* Non-root: look up the entry in the parent directory. */
    char parent_dir[MAX_PATH_LEN];
    get_parent_path(path, parent_dir, sizeof(parent_dir));
    const char *entry_name = get_file_name(path);

    DirEntry entry;
    int lookup_ret = dir_cache_lookup_entry(priv, parent_dir,
                                            entry_name, &entry);

    if (lookup_ret != 0) {
        if (!(mode & AXL_FS_OPEN_CREATE)) {
            return AXL_FS_ERR_NOT_FOUND;
        }
        if (priv->read_only) {
            return AXL_FS_ERR_WRITE_PROTECTED;
        }
        /* CREATE: file or directory per EFI Attributes (translated by
           the SDK thunk into AXL_FS_ATTR_DIRECTORY in `attributes`). */
        const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
        size_t op_status = 0;
        if (attributes & AXL_FS_ATTR_DIRECTORY) {
            if (ops->mkdir(priv, path, &op_status) != 0) {
                return AXL_FS_ERR_IO;
            }
            if (op_status != 200 && op_status != 201) {
                return AXL_FS_ERR_IO;
            }
            entry.is_dir = true;
            entry.size   = 0;
        } else {
            if (ops->create_empty(priv, path, &op_status) != 0) {
                return AXL_FS_ERR_IO;
            }
            entry.is_dir = false;
            entry.size   = 0;
        }
        dir_cache_invalidate(priv, parent_dir);
    }

    AxlFsProviderFile *fh = webfs_alloc_file(priv, path,
                                             entry.is_dir, entry.size);
    if (fh == NULL) return AXL_FS_ERR_NO_MEMORY;

    /* Best-effort integrity check: mark "want digest" so the first
       sequential Read can pick the `Digest: sha-256=<hex>` header
       out of the Range response — no extra round-trip on Open.
       Once we have the header, digest_active flips on and reads
       accumulate into digest_ctx. Mismatch latches
       AXL_FS_ERR_VOLUME_CORRUPTED on Close. */
    if (!entry.is_dir) {
        fh->digest_want = true;
    }

    *out = fh;
    *out_is_dir = entry.is_dir;
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_close(AxlFsProviderFile *fh)
{
    AxlFsStatus rc = AXL_FS_OK;

    /* Drain buffered Writes before teardown. flush_put_buf resets
       buffer state regardless of outcome so we never leak put_buf
       even on PUT failure. */
    if (fh->put_dirty) {
        AxlFsStatus flush_rc = flush_put_buf(fh);
        if (flush_rc != AXL_FS_OK) rc = flush_rc;
    }

    /* Integrity verification at end-of-handle. */
    if (fh->digest_active && fh->digest_ctx != NULL) {
        if (!fh->digest_failed && fh->digest_consumed == fh->file_size) {
            const char *got = axl_checksum_get_string(fh->digest_ctx);
            if (got == NULL ||
                axl_strcasecmp(got, fh->digest_expected) != 0) {
                axl_error("digest mismatch on %s: expected %s, got %s",
                          fh->path, fh->digest_expected,
                          got ? got : "(null)");
                rc = AXL_FS_ERR_VOLUME_CORRUPTED;
            } else {
                axl_info("digest verified: %s sha-256=%s",
                         fh->path, got);
            }
        }
        axl_checksum_free(fh->digest_ctx);
        fh->digest_ctx = NULL;
    }

    if (fh->read_ahead_buf != NULL) axl_free(fh->read_ahead_buf);
    if (fh->dir_entries    != NULL) axl_free(fh->dir_entries);
    axl_free(fh);
    return rc;
}

// ---------------------------------------------------------------------------
// read (regular files only)
// ---------------------------------------------------------------------------

/// Feed the freshly-read bytes into the streaming digest, but only
/// if the position right before the read was the expected next
/// sequential offset. Any non-sequential read latches digest_failed.
static void
digest_consume(AxlFsProviderFile *fh, uint64_t pre_read_position,
               const void *bytes, size_t len)
{
    if (!fh->digest_active || fh->digest_failed) return;
    if (pre_read_position != fh->digest_consumed) {
        fh->digest_failed = true;
        return;
    }
    axl_checksum_update(fh->digest_ctx, bytes, len);
    fh->digest_consumed += len;
}

static AxlFsStatus
webfs_read(AxlFsProviderFile *fh, void *buf, size_t *inout_size)
{
    WebFsPrivate *priv = fh->priv;
    if (buf == NULL || inout_size == NULL) return AXL_FS_ERR_INVALID;
    if (fh->is_dir) return AXL_FS_ERR_IS_DIR;

    if (fh->position >= fh->file_size) {
        *inout_size = 0;
        return AXL_FS_OK;
    }

    size_t wanted = *inout_size;
    if (fh->position + wanted > fh->file_size) {
        wanted = (size_t)(fh->file_size - fh->position);
    }
    uint64_t pre_read_position = fh->position;

    /* Read-ahead hit. */
    if (fh->read_ahead_buf != NULL && fh->read_ahead_len > 0 &&
        fh->position >= fh->read_ahead_start &&
        fh->position < fh->read_ahead_start + fh->read_ahead_len) {
        size_t offset  = (size_t)(fh->position - fh->read_ahead_start);
        size_t avail   = fh->read_ahead_len - offset;
        size_t to_copy = (wanted < avail) ? wanted : avail;
        axl_memcpy(buf, fh->read_ahead_buf + offset, to_copy);
        digest_consume(fh, pre_read_position, buf, to_copy);
        fh->position += to_copy;
        *inout_size = to_copy;
        return AXL_FS_OK;
    }

    /* Read-ahead miss — pull bytes via the active protocol. */
    bool use_readahead = (fh->read_ahead_buf != NULL &&
                          wanted < READAHEAD_BUF_SIZE);
    size_t fetch_size = use_readahead ? READAHEAD_BUF_SIZE : wanted;
    if (fh->position + fetch_size > fh->file_size) {
        fetch_size = (size_t)(fh->file_size - fh->position);
    }

    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    void  *dst = use_readahead ? fh->read_ahead_buf : buf;
    size_t got = 0;
    char  *digest_slot = NULL;
    size_t digest_slot_size = 0;
    if (fh->digest_want && !fh->digest_active && !fh->digest_failed &&
        fh->position == 0) {
        digest_slot      = fh->digest_expected;
        digest_slot_size = sizeof(fh->digest_expected);
        fh->digest_expected[0] = '\0';
    }
    if (ops->read_range(priv, fh->path, fh->position, fetch_size,
                        dst, &got,
                        digest_slot, digest_slot_size) != 0) {
        *inout_size = 0;
        return AXL_FS_ERR_IO;
    }
    if (digest_slot != NULL && fh->digest_expected[0] != '\0') {
        fh->digest_ctx = axl_checksum_new(AXL_CHECKSUM_SHA256);
        if (fh->digest_ctx != NULL) {
            fh->digest_active   = true;
            fh->digest_consumed = 0;
        }
        fh->digest_want = false;
    } else if (digest_slot != NULL) {
        /* Server didn't advertise a digest — skip validation. */
        fh->digest_want = false;
    }

    if (use_readahead) {
        fh->read_ahead_start = fh->position;
        fh->read_ahead_len   = got;
        size_t user_copy = (wanted < got) ? wanted : got;
        axl_memcpy(buf, fh->read_ahead_buf, user_copy);
        digest_consume(fh, pre_read_position, buf, user_copy);
        fh->position += user_copy;
        *inout_size = user_copy;
    } else {
        digest_consume(fh, pre_read_position, buf, got);
        fh->position += got;
        *inout_size = got;
        fh->read_ahead_len = 0;
    }
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// read_dir
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_read_dir(AxlFsProviderFile *fh, AxlFsEntry *out, bool *out_end)
{
    WebFsPrivate *priv = fh->priv;
    if (out == NULL || out_end == NULL) return AXL_FS_ERR_INVALID;
    if (!fh->is_dir) return AXL_FS_ERR_NOT_DIR;

    /* Lazy-load directory listing (copy from cache for stability). */
    if (!fh->dir_loaded) {
        DirEntry *cache_entries = NULL;
        size_t    cache_count   = 0;
        int ret = dir_cache_fetch(priv, fh->path,
                                  &cache_entries, &cache_count);
        if (ret != 0) return AXL_FS_ERR_IO;
        fh->dir_entries = axl_calloc(cache_count, sizeof(DirEntry));
        if (fh->dir_entries == NULL) return AXL_FS_ERR_NO_MEMORY;
        axl_memcpy(fh->dir_entries, cache_entries,
                   cache_count * sizeof(DirEntry));
        fh->dir_entry_count = cache_count;
        fh->dir_loaded      = true;
        fh->dir_read_index  = 0;
    }

    if (fh->dir_read_index >= fh->dir_entry_count) {
        *out_end = true;
        return AXL_FS_OK;
    }

    DirEntry *e = &fh->dir_entries[fh->dir_read_index];
    fh->dir_read_index++;

    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    axl_strlcpy(out->name, e->name, sizeof(out->name));
    out->size        = e->size;
    out->mtime_unix  = 0;     /* server's ISO-8601 modified isn't decoded yet */
    out->attributes  = e->is_dir ? AXL_FS_ATTR_DIRECTORY : 0u;
    *out_end = false;
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// write — staged into put_buf, drained on close/flush
// ---------------------------------------------------------------------------

static AxlFsStatus
flush_put_buf(AxlFsProviderFile *fh)
{
    if (!fh->put_dirty) return AXL_FS_OK;
    WebFsPrivate *priv = fh->priv;
    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);

    /* Compute SHA-256 of the accumulator so the server can verify
       end-to-end on PUT (SDK Content-Digest validation). */
    char    digest_hex[65];
    uint8_t digest_raw[32];
    if (axl_compute_checksum_digest(AXL_CHECKSUM_SHA256,
                                    fh->put_buf, fh->put_buf_used,
                                    digest_raw, sizeof(digest_raw))
            == AXL_OK) {
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < 32; i++) {
            digest_hex[i * 2]     = hex[digest_raw[i] >> 4];
            digest_hex[i * 2 + 1] = hex[digest_raw[i] & 0xF];
        }
        digest_hex[64] = '\0';
    } else {
        digest_hex[0] = '\0';
    }

    size_t status = 0;
    int rc = ops->write_full(priv, fh->path, fh->put_buf,
                             fh->put_buf_used,
                             digest_hex[0] != '\0' ? digest_hex : NULL,
                             &status);

    axl_free(fh->put_buf);
    fh->put_buf      = NULL;
    fh->put_buf_cap  = 0;
    fh->put_buf_used = 0;
    fh->put_dirty    = false;

    if (rc != 0 ||
        (status != 200 && status != 201 && status != 204)) {
        return AXL_FS_ERR_IO;
    }

    /* Server view changed — drop the parent listing's cached entry. */
    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    dir_cache_invalidate(priv, parent_dir);
    return AXL_FS_OK;
}

static AxlFsStatus
webfs_write(AxlFsProviderFile *fh, const void *buf, size_t *inout_size)
{
    WebFsPrivate *priv = fh->priv;
    if (buf == NULL || inout_size == NULL) return AXL_FS_ERR_INVALID;
    if (fh->is_dir) return AXL_FS_ERR_IS_DIR;
    if (priv->read_only) return AXL_FS_ERR_WRITE_PROTECTED;
    if (*inout_size == 0) return AXL_FS_OK;

    if (!fh->put_dirty) {
        fh->put_buf = axl_malloc(PUT_BUF_INITIAL);
        if (fh->put_buf == NULL) return AXL_FS_ERR_NO_MEMORY;
        fh->put_buf_cap  = PUT_BUF_INITIAL;
        fh->put_buf_used = 0;
        fh->put_dirty    = true;
        if (fh->digest_active) fh->digest_failed = true;
    }

    size_t need = fh->put_buf_used + *inout_size;
    if (need > PUT_BUF_MAX) return AXL_FS_ERR_NO_SPACE;
    if (need > fh->put_buf_cap) {
        size_t new_cap = fh->put_buf_cap * 2;
        while (new_cap < need) new_cap *= 2;
        if (new_cap > PUT_BUF_MAX) new_cap = PUT_BUF_MAX;
        uint8_t *nb = axl_realloc(fh->put_buf, new_cap);
        if (nb == NULL) return AXL_FS_ERR_NO_MEMORY;
        fh->put_buf     = nb;
        fh->put_buf_cap = new_cap;
    }

    axl_memcpy(fh->put_buf + fh->put_buf_used, buf, *inout_size);
    fh->put_buf_used += *inout_size;
    fh->position     += *inout_size;
    if (fh->position > fh->file_size) fh->file_size = fh->position;
    fh->read_ahead_len = 0;     /* invalidate */
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// seek
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_seek(AxlFsProviderFile *fh, uint64_t position)
{
    if (fh->is_dir) {
        if (position == 0) {
            fh->dir_read_index = 0;
            return AXL_FS_OK;
        }
        return AXL_FS_ERR_UNSUPPORTED;
    }
    /* Any seek breaks the sequential-read invariant. */
    if (fh->digest_active) fh->digest_failed = true;
    fh->position = (position == (uint64_t)-1) ? fh->file_size : position;
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// del
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_del(AxlFsProviderFile *fh)
{
    WebFsPrivate *priv = fh->priv;
    if (priv->read_only) return AXL_FS_ERR_WRITE_PROTECTED;

    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    size_t del_status = 0;
    int ret = ops->remove(priv, fh->path, &del_status);

    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    dir_cache_invalidate(priv, parent_dir);

    /* 200/204 = removed cleanly; 404 = already gone (treat as success
       per the JSON path's historical behavior). */
    if (ret != 0 ||
        (del_status != 200 && del_status != 204 && del_status != 404)) {
        return AXL_FS_ERR_IO;
    }
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// flush
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_flush(AxlFsProviderFile *fh)
{
    if (fh->put_dirty) return flush_put_buf(fh);
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// get_info / set_info
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_get_info(AxlFsProviderFile *fh, AxlFsEntry *out)
{
    WebFsPrivate *priv = fh->priv;
    if (out == NULL) return AXL_FS_ERR_INVALID;

    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    out->mtime_unix  = 0;

    DirEntry entry;
    axl_memset(&entry, 0, sizeof(entry));

    if (fh->is_root) {
        out->name[0]    = '\0';
        out->size       = 0;
        out->attributes = AXL_FS_ATTR_DIRECTORY;
        return AXL_FS_OK;
    }

    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    const char *name = get_file_name(fh->path);

    if (dir_cache_lookup_entry(priv, parent_dir, name, &entry) == 0) {
        axl_strlcpy(out->name, entry.name, sizeof(out->name));
        out->size       = entry.size;
        out->attributes = entry.is_dir ? AXL_FS_ATTR_DIRECTORY : 0u;
    } else {
        /* Cache miss fallback: use what we know on the handle. */
        axl_strlcpy(out->name, name, sizeof(out->name));
        out->size       = fh->file_size;
        out->attributes = fh->is_dir ? AXL_FS_ATTR_DIRECTORY : 0u;
    }
    return AXL_FS_OK;
}

static AxlFsStatus
webfs_set_info(AxlFsProviderFile *fh, const AxlFsEntry *in)
{
    WebFsPrivate *priv = fh->priv;
    if (in == NULL) return AXL_FS_ERR_INVALID;
    if (priv->read_only) return AXL_FS_ERR_WRITE_PROTECTED;

    /* Treat in->name change as rename. The thunk has already
       decoded the trailing CHAR16 name into UTF-8. */
    const char *old_name = get_file_name(fh->path);
    if (axl_streql(old_name, in->name)) {
        return AXL_FS_OK;     /* attribute change without rename */
    }

    const WebfsProtocolOps *ops = webfs_protocol_ops(priv->protocol);
    size_t rename_status = 0;
    int ret = ops->rename(priv, fh->path, in->name, &rename_status);
    if (ret != 0 ||
        (rename_status != 200 && rename_status != 201 &&
         rename_status != 204)) {
        return AXL_FS_ERR_IO;
    }

    char parent_dir[MAX_PATH_LEN];
    get_parent_path(fh->path, parent_dir, sizeof(parent_dir));
    dir_cache_invalidate(priv, parent_dir);
    /* The path on the handle now points to the renamed entry. */
    axl_snprintf(fh->path, sizeof(fh->path), "%s%s", parent_dir, in->name);
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// volume_info — synthesizes EFI_FILE_SYSTEM_INFO from the mount state
// ---------------------------------------------------------------------------

static AxlFsStatus
webfs_volume_info(void *backend_ctx, AxlFsProviderVolumeInfo *out)
{
    WebFsPrivate *priv = backend_ctx;
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_PROVIDER_VERSION;
    out->read_only   = priv->read_only;
    /* Server backing — we don't query free space. Synthetic huge
       value so UEFI Shell `cp` doesn't refuse the write upfront
       (the actual write/PUT exchange determines success). */
    out->volume_size = (uint64_t)-1;
    out->free_space  = (uint64_t)-1;
    out->block_size  = 512;
    axl_strlcpy(out->label, VOLUME_LABEL_UTF8, sizeof(out->label));
    return AXL_FS_OK;
}

// ---------------------------------------------------------------------------
// Provider vtable (caller-owned, lives in BSS)
// ---------------------------------------------------------------------------

const AxlFsProvider webfs_provider = {
    .struct_size   = sizeof(AxlFsProvider),
    .version       = AXL_FS_PROVIDER_VERSION,
    .open          = webfs_open,
    .close         = webfs_close,
    .read          = webfs_read,
    .read_dir      = webfs_read_dir,
    .write         = webfs_write,
    .seek          = webfs_seek,
    .del           = webfs_del,
    .flush         = webfs_flush,
    .get_info      = webfs_get_info,
    .set_info      = webfs_set_info,
    .volume_info   = webfs_volume_info,
    .default_label = VOLUME_LABEL_UTF8,
    /* backend_ctx filled at publish time by mount_setup. */
};
