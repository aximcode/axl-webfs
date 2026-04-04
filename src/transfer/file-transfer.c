/** @file
  FileTransferLib -- Volume enumeration, streaming file I/O, file operations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
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

/// Build a full "vol:path" string with forward slashes converted to backslashes.
static void build_path(const FtVolume *vol, const char *sub_path,
                       char *out, size_t out_size)
{
    axl_snprintf(out, out_size, "%s:%s", vol->name, sub_path);
    for (char *p = out; *p; p++) {
        if (*p == '/')
            *p = '\\';
    }
}

// ----------------------------------------------------------------------------
// Streaming read
// ----------------------------------------------------------------------------

int ft_open_read(FtVolume *vol, const char *path, uint64_t offset,
                 FtProgressCb cb, void *ctx, FtReadCtx *rctx)
{
    char full[512];
    build_path(vol, path, full, sizeof(full));

    AxlStream *s = axl_fopen(full, "r");
    if (s == NULL)
        return -1;

    /* Get file size via axl_file_info */
    AxlFileInfo fi;
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
// Streaming write
// ----------------------------------------------------------------------------

int ft_open_write(FtVolume *vol, const char *path, FtProgressCb cb,
                  void *ctx, FtWriteCtx *wctx)
{
    /* Create parent directories if needed */
    char parent[512];
    axl_strlcpy(parent, path, sizeof(parent));
    size_t len = axl_strlen(parent);
    size_t last_sep = 0;
    for (size_t i = 0; i < len; i++) {
        if (parent[i] == '/' || parent[i] == '\\')
            last_sep = i;
    }
    if (last_sep > 0) {
        parent[last_sep] = '\0';
        ft_mkdir(vol, parent);
    }

    char full[512];
    build_path(vol, path, full, sizeof(full));

    AxlStream *s = axl_fopen(full, "w");
    if (s == NULL)
        return -1;

    wctx->stream = s;
    wctx->bytes_written = 0;
    wctx->progress_cb = cb;
    wctx->progress_ctx = ctx;
    return 0;
}

int ft_write_chunk(FtWriteCtx *wctx, const void *data, size_t len)
{
    size_t wrote = axl_fwrite(data, 1, len, wctx->stream);
    if (wrote != len)
        return -1;
    wctx->bytes_written += wrote;
    if (wctx->progress_cb != NULL)
        wctx->progress_cb((size_t)wctx->bytes_written, SIZE_MAX,
                          wctx->progress_ctx);
    return 0;
}

void ft_close_write(FtWriteCtx *wctx)
{
    if (wctx->stream != NULL) {
        axl_fflush(wctx->stream);
        axl_fclose(wctx->stream);
        wctx->stream = NULL;
    }
}

// ----------------------------------------------------------------------------
// File operations
// ----------------------------------------------------------------------------

int ft_delete(FtVolume *vol, const char *path)
{
    char full[512];
    build_path(vol, path, full, sizeof(full));
    return axl_file_delete(full);
}

int ft_mkdir(FtVolume *vol, const char *path)
{
    char full[512];
    build_path(vol, path, full, sizeof(full));
    return axl_dir_mkdir(full);
}

int ft_get_file_size(FtVolume *vol, const char *path, uint64_t *size)
{
    char full[512];
    build_path(vol, path, full, sizeof(full));
    AxlFileInfo fi;
    if (axl_file_info(full, &fi) != 0)
        return -1;
    *size = fi.size;
    return 0;
}

int ft_is_dir(FtVolume *vol, const char *path, bool *is_dir)
{
    char full[512];
    build_path(vol, path, full, sizeof(full));
    AxlFileInfo fi;
    if (axl_file_info(full, &fi) != 0)
        return -1;
    *is_dir = fi.is_dir;
    return 0;
}
