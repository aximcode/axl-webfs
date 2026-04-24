/** @file
  FileTransferLib -- Volume enumeration, streaming file I/O, directory listing.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#ifndef FILE_TRANSFER_LIB_H_
#define FILE_TRANSFER_LIB_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FT_CHUNK_SIZE    (8 * 1024)
#define FT_MAX_VOLUMES   8

typedef struct {
    void  *handle;   /* opaque filesystem handle */
    char   name[16]; /* volume name, e.g. "fs0" */
} FtVolume;

typedef void (*FtProgressCb)(size_t bytes_transferred, size_t total_bytes, void *ctx);

/* Forward declare AxlStream -- defined in axl/axl-io.h */
typedef struct AxlStream AxlStream;

typedef struct {
    AxlStream   *stream;
    uint64_t     file_size;
    uint64_t     position;
    FtProgressCb progress_cb;
    void        *progress_ctx;
} FtReadCtx;

typedef struct {
    AxlStream   *stream;
    uint64_t     bytes_written;
    FtProgressCb progress_cb;
    void        *progress_ctx;
} FtWriteCtx;

int    ft_init(void);
size_t ft_get_volume_count(void);
int    ft_get_volume(size_t index, FtVolume *vol);
int    ft_find_volume(const char *name, FtVolume *vol);

int    ft_list_volumes(bool as_json, char *buf, size_t buf_size, size_t *written);
int    ft_list_dir(FtVolume *vol, const char *path, bool as_json, char *buf, size_t buf_size, size_t *written);

int    ft_open_read(FtVolume *vol, const char *path, uint64_t offset, FtProgressCb cb, void *ctx, FtReadCtx *rctx);
int    ft_read_chunk(FtReadCtx *rctx, void *buf, size_t buf_size, size_t *bytes_read);
void   ft_close_read(FtReadCtx *rctx);

int    ft_open_write(FtVolume *vol, const char *path, FtProgressCb cb, void *ctx, FtWriteCtx *wctx);
int    ft_write_chunk(FtWriteCtx *wctx, const void *data, size_t len);
void   ft_close_write(FtWriteCtx *wctx);

int    ft_delete(FtVolume *vol, const char *path);
int    ft_mkdir(FtVolume *vol, const char *path);
int    ft_get_file_size(FtVolume *vol, const char *path, uint64_t *size);
int    ft_is_dir(FtVolume *vol, const char *path, bool *is_dir);

#endif
