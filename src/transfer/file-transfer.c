/** @file
  FileTransferLib -- Volume enumeration, streaming file I/O, file operations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "transfer/file-transfer.h"
#include <axl.h>


// ----------------------------------------------------------------------------
// Volume table
// ----------------------------------------------------------------------------

static FtVolume mVolumes[FT_MAX_VOLUMES];
static size_t   mVolumeCount = 0;

int ft_init(void)
{
    mVolumeCount = 0;
    /* AxlVolume and FtVolume have identical layout */
    return axl_volume_enumerate((AxlVolume *)mVolumes, FT_MAX_VOLUMES, &mVolumeCount);
}

size_t ft_get_volume_count(void)
{
    return mVolumeCount;
}

int ft_get_volume(size_t index, FtVolume *vol)
{
    if (index >= mVolumeCount || vol == NULL)
        return -1;
    *vol = mVolumes[index];
    return 0;
}

int ft_find_volume(const char *name, FtVolume *vol)
{
    for (size_t i = 0; i < mVolumeCount; i++) {
        if (axl_streql(mVolumes[i].name, name)) {
            *vol = mVolumes[i];
            return 0;
        }
    }
    return -1;
}

// ----------------------------------------------------------------------------
// Path helpers
// ----------------------------------------------------------------------------

int ft_build_path(const FtVolume *vol, const char *sub_path,
                  char *out, size_t out_size)
{
    int n = axl_snprintf(out, out_size, "%s:%s", vol->name, sub_path);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return -1;      /* refused, never cut: a cut path names another */
    }
    for (char *p = out; *p; p++) {
        if (*p == '/')
            *p = '\\';
    }
    return 0;
}

bool ft_path_components_ok(const char *sub_path)
{
    const char *start = sub_path;
    for (const char *q = sub_path; ; q++) {
        if (*q == '/' || *q == '\0') {
            size_t len = (size_t)(q - start);
            if (len > 0 && (start[0] == ' ' || start[len - 1] == ' ' ||
                            start[len - 1] == '.'))
                return false;
            if (*q == '\0')
                return true;
            start = q + 1;
        }
    }
}

// ----------------------------------------------------------------------------
// Streaming read
// ----------------------------------------------------------------------------

int ft_open_read(FtVolume *vol, const char *path, uint64_t offset,
                 FtProgressCb cb, void *ctx, FtReadCtx *rctx)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, path, full, sizeof(full)) != 0)
        return -1;

    AxlStream *s = axl_fopen(full, "r");
    if (s == NULL)
        return -1;

    /* Get file size via axl_file_info */
    AxlFsEntry fi;
    if (axl_file_info(full, &fi) != 0) {
        axl_fclose(s);
        return -1;
    }

    if (offset > 0)
        axl_fseek(s, (long)offset, 0);

    rctx->stream = s;
    rctx->file_size = fi.size;
    rctx->position = offset;
    rctx->progress_cb = cb;
    rctx->progress_ctx = ctx;
    return 0;
}

int ft_read_chunk(FtReadCtx *rctx, void *buf, size_t buf_size,
                  size_t *bytes_read)
{
    size_t got = axl_fread(buf, 1, buf_size, rctx->stream);
    *bytes_read = got;
    rctx->position += got;
    if (rctx->progress_cb != NULL)
        rctx->progress_cb((size_t)rctx->position, (size_t)rctx->file_size,
                          rctx->progress_ctx);
    return (got > 0 || axl_feof(rctx->stream)) ? 0 : -1;
}

void ft_close_read(FtReadCtx *rctx)
{
    if (rctx->stream != NULL) {
        axl_fclose(rctx->stream);
        rctx->stream = NULL;
    }
}

// ----------------------------------------------------------------------------
// File operations
// ----------------------------------------------------------------------------

int ft_delete(FtVolume *vol, const char *path)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, path, full, sizeof(full)) != 0)
        return -1;
    return axl_file_delete(full);
}

int ft_rmdir(FtVolume *vol, const char *path)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, path, full, sizeof(full)) != 0)
        return -1;
    return axl_dir_rmdir(full);
}

int ft_mkdir(FtVolume *vol, const char *path)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, path, full, sizeof(full)) != 0)
        return -1;
    return axl_dir_mkdir(full);
}

/* WebDAV MOVE. Thin wrapper around axl_file_move: same-directory
   on-volume is an atomic rename; cross-directory falls back to
   chunked stream copy + source delete inside the SDK. Cross-volume
   is also supported via the copy path. */
int ft_move(FtVolume *src_vol, const char *src_path,
            FtVolume *dst_vol, const char *dst_path)
{
    char src_full[FT_PATH_MAX];
    char dst_full[FT_PATH_MAX];
    if (ft_build_path(src_vol, src_path, src_full, sizeof(src_full)) != 0 ||
        ft_build_path(dst_vol, dst_path, dst_full, sizeof(dst_full)) != 0)
        return -1;
    return axl_file_move(src_full, dst_full);
}

int ft_get_file_size(FtVolume *vol, const char *path, uint64_t *size)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, path, full, sizeof(full)) != 0)
        return -1;
    AxlFsEntry fi;
    if (axl_file_info(full, &fi) != 0)
        return -1;
    *size = fi.size;
    return 0;
}

int ft_is_dir(FtVolume *vol, const char *path, bool *is_dir)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, path, full, sizeof(full)) != 0)
        return -1;
    AxlFsEntry fi;
    if (axl_file_info(full, &fi) != 0)
        return -1;
    *is_dir = axl_fs_entry_is_dir(&fi);
    return 0;
}

int ft_stat(FtVolume *vol, const char *path, AxlFsEntry *info)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, path, full, sizeof(full)) != 0)
        return -1;
    return axl_file_info(full, info);
}
