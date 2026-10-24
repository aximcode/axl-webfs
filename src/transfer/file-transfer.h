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
#include <axl/axl-fs.h>

#define FT_CHUNK_SIZE    (8 * 1024)
#define FT_MAX_VOLUMES   8

/* FtVolume is intentionally an alias for AxlVolume — the layout MUST
   match because file-transfer.c casts mVolumes directly to AxlVolume*
   for axl_volume_enumerate. Keeping a hand-rolled struct here led to
   silent corruption when axl-sdk added the device_path field (entries
   1+ in the listing got shifted bytes). Use the SDK type directly so
   future field additions stay in sync. */
typedef AxlVolume FtVolume;

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

int    ft_init(void);
size_t ft_get_volume_count(void);
int    ft_get_volume(size_t index, FtVolume *vol);
int    ft_find_volume(const char *name, FtVolume *vol);

/* Entries ft_list_dir leaves out: return true to hide @p name, found in
   volume-relative directory @p dir_path. One filter, set by whoever
   owns files that are not yet resources (the WebDAV PUT temps); NULL
   clears it. */
typedef bool (*FtListFilter)(const FtVolume *vol, const char *dir_path,
                             const char *name);
void   ft_set_list_filter(FtListFilter hide);

/* "<vol>:<sub>" with '\\' separators, the UEFI path ft_* operate on, in
   @p out (FT_PATH_MAX bytes is always enough for a path that fits).
   -1 -- and "" -- when it does not fit: refused, never cut. */
#define FT_PATH_MAX  512
int    ft_build_path(const FtVolume *vol, const char *sub_path,
                     char *out, size_t out_size);

/* Whether every '/'-separated component of @p sub_path is a name FAT
   could store as spelled: none starts with a blank or ends with a blank
   or a period (so "." and ".." are refused too). The FAT driver TRIMS
   those before it looks a name up, so "<x>." or " <x>" would reach the
   entry <x> under a spelling no exact-name check matches. */
bool   ft_path_components_ok(const char *sub_path);

int    ft_list_volumes(bool as_json, char *buf, size_t buf_size, size_t *written);
int    ft_list_dir(FtVolume *vol, const char *path, bool as_json, bool read_only, char *buf, size_t buf_size, size_t *written);

int    ft_open_read(FtVolume *vol, const char *path, uint64_t offset, FtProgressCb cb, void *ctx, FtReadCtx *rctx);
int    ft_read_chunk(FtReadCtx *rctx, void *buf, size_t buf_size, size_t *bytes_read);
void   ft_close_read(FtReadCtx *rctx);

int    ft_delete(FtVolume *vol, const char *path);
int    ft_rmdir(FtVolume *vol, const char *path);
int    ft_mkdir(FtVolume *vol, const char *path);
int    ft_move(FtVolume *src_vol, const char *src_path,
               FtVolume *dst_vol, const char *dst_path);
int    ft_get_file_size(FtVolume *vol, const char *path, uint64_t *size);
int    ft_is_dir(FtVolume *vol, const char *path, bool *is_dir);
int    ft_stat(FtVolume *vol, const char *path, AxlFsEntry *info);

#endif
