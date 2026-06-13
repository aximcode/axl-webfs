/** @file
  axl-webfs -- WebDAV class-1 + MOVE adapter onto the ft_* helpers.

  All protocol bits (verb dispatch, PROPFIND XML, Depth/Destination/
  Overwrite parsing, DAV: 1 advertisement) live in axl-sdk; this file
  just maps AxlWebDavOps callbacks onto the FtVolume / ft_* layer.

  URL layout: /dav/<vol>/<path>. Clients (Finder, Explorer, davfs2,
  cadaver) see the mount root /dav as a collection containing one
  virtual subdirectory per UEFI volume — list_dir("/") returns the
  volume names; everything under /dav/<vol>/ maps to that volume's
  filesystem.

  Streaming: GET goes through axl_http_response_set_streamer via
  the read_open/read_chunk/read_close trio; PUT goes through the
  upload-route chunk handler via write_open/write_chunk/write_close,
  inheriting the abort contract.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "serve/webfs-dav.h"
#include "transfer/file-transfer.h"

#include <axl.h>

AXL_LOG_DOMAIN("webfs-dav");

/* Forward decl into webfs-serve.c — both files are compiled into the
   serve DXE driver, so the linker resolves this without a header.
   Sharing keeps the SHA-256 cache (m_digest_cache) in one place. */
extern int compute_file_digest(FtVolume *volume, const char *sub_path,
                               uint64_t file_size, char *out_hex);

// ----------------------------------------------------------------------------
// Path parsing
// ----------------------------------------------------------------------------

/* Parse a WebDAV-relative path "/<vol>/<sub>" (or "/" / "/<vol>"
   variants). Returns one of three categories so callers know whether
   to treat the path as the virtual root, a volume root, or a real
   filesystem object.

   Layout summary:
     PATH_ROOT     "/"           vol=N/A  sub=N/A
     PATH_VOLUME   "/<vol>"      vol=set  sub="/"
     PATH_OBJECT   "/<vol>/x"    vol=set  sub="/x"

   The volume root case (PATH_VOLUME) is distinguished from
   PATH_OBJECT with sub="/" because a missing volume should map to
   404 even when sub looks like a valid root path. */
typedef enum {
    PATH_ROOT,
    PATH_VOLUME,
    PATH_OBJECT,
    PATH_INVALID,           ///< unknown volume name
} DavPathKind;

static DavPathKind
parse_dav_path(const char *url, FtVolume *vol,
               char *sub, size_t sub_size)
{
    if (url == NULL || url[0] == '\0' ||
        (url[0] == '/' && url[1] == '\0')) {
        return PATH_ROOT;
    }

    const char *p = url;
    if (*p == '/')
        p++;

    char vol_name[16];
    size_t i = 0;
    while (*p != '/' && *p != '\0' && i < sizeof(vol_name) - 1) {
        vol_name[i++] = *p++;
    }
    vol_name[i] = '\0';

    if (ft_find_volume(vol_name, vol) != 0)
        return PATH_INVALID;

    if (*p == '\0') {
        axl_strlcpy(sub, "/", sub_size);
        return PATH_VOLUME;
    }

    /* p now points at '/' starting the sub-path. Copy as-is. */
    axl_strlcpy(sub, p, sub_size);
    return PATH_OBJECT;
}

// ----------------------------------------------------------------------------
// PROPFIND callbacks
// ----------------------------------------------------------------------------

static int
dav_list_dir(void *user, const char *path, AxlFsEntry *out,
             size_t max, size_t *count)
{
    (void)user;
    *count = 0;

    FtVolume vol;
    char     sub[512];
    DavPathKind kind = parse_dav_path(path, &vol, sub, sizeof(sub));

    if (kind == PATH_INVALID)
        return AXL_ERR;

    /* Mount root → one virtual entry per UEFI volume. */
    if (kind == PATH_ROOT) {
        size_t n = ft_get_volume_count();
        if (n > max) n = max;
        for (size_t j = 0; j < n; j++) {
            FtVolume v;
            ft_get_volume(j, &v);
            axl_memset(&out[j], 0, sizeof(out[j]));
            out[j].struct_size = sizeof(out[j]);
            out[j].version     = AXL_FS_ENTRY_VERSION;
            axl_strlcpy(out[j].name, v.name, sizeof(out[j].name));
            out[j].attributes  = AXL_FS_ATTR_DIRECTORY;
        }
        *count = n;
        return AXL_OK;
    }

    /* Real directory on a volume (incl. volume root sub="/"). */
    char dir_full[512];
    axl_snprintf(dir_full, sizeof(dir_full), "%s:%s", vol.name, sub);
    for (char *q = dir_full; *q; q++)
        if (*q == '/') *q = '\\';

    AxlDir *dir = axl_dir_open(dir_full);
    if (dir == NULL)
        return AXL_ERR;

    AxlFsEntry e;
    size_t n = 0;
    while (n < max && axl_dir_read(dir, &e)) {
        if (axl_streql(e.name, ".") || axl_streql(e.name, ".."))
            continue;
        out[n] = e;     /* propagate struct_size/version/attributes/etc. */
        n++;
    }
    axl_dir_close(dir);
    *count = n;
    return AXL_OK;
}

static int
dav_stat(void *user, const char *path, AxlFsEntry *out)
{
    (void)user;
    axl_memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;

    FtVolume vol;
    char     sub[512];
    DavPathKind kind = parse_dav_path(path, &vol, sub, sizeof(sub));

    switch (kind) {
        case PATH_ROOT:
            /* SDK fills "/" when name is empty for the mount root. */
            out->attributes = AXL_FS_ATTR_DIRECTORY;
            return AXL_OK;
        case PATH_VOLUME:
            axl_strlcpy(out->name, vol.name, sizeof(out->name));
            out->attributes = AXL_FS_ATTR_DIRECTORY;
            return AXL_OK;
        case PATH_OBJECT: {
            AxlFsEntry fi;
            if (ft_stat(&vol, sub, &fi) != 0)
                return AXL_ERR;
            /* basename for name field. */
            const char *base = sub;
            for (const char *q = sub; *q; q++)
                if (*q == '/' && q[1] != '\0')
                    base = q + 1;
            axl_strlcpy(out->name, base, sizeof(out->name));
            /* Propagate the full attribute bitmask so HIDDEN/SYSTEM/
               READ_ONLY/ARCHIVE bits surface in PROPFIND. Pre-migration
               this was a bool collapse and only DIRECTORY round-tripped. */
            out->attributes = fi.attributes;
            out->size       = axl_fs_entry_is_dir(&fi) ? 0 : fi.size;
            out->alloc_size = fi.alloc_size;
            out->mtime_unix = fi.mtime_unix;
            return AXL_OK;
        }
        case PATH_INVALID:
        default:
            return AXL_ERR;
    }
}

// ----------------------------------------------------------------------------
// Streaming read (GET / HEAD)
// ----------------------------------------------------------------------------

typedef struct {
    FtReadCtx ctx;
    bool      open;
} DavReadCtx;

static int
dav_read_open(void *user, const char *path, uint64_t offset, void **out_ctx)
{
    (void)user;
    *out_ctx = NULL;

    FtVolume vol;
    char     sub[512];
    if (parse_dav_path(path, &vol, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;

    DavReadCtx *r = axl_calloc(1, sizeof(*r));
    if (r == NULL)
        return AXL_ERR;
    if (ft_open_read(&vol, sub, offset, NULL, NULL, &r->ctx) != 0) {
        axl_free(r);
        return AXL_ERR;
    }
    r->open = true;
    *out_ctx = r;
    return AXL_OK;
}

static int
dav_read_chunk(void *ctx, void *buf, size_t buf_size, size_t *bytes_read)
{
    DavReadCtx *r = ctx;
    size_t got = 0;
    if (ft_read_chunk(&r->ctx, buf, buf_size, &got) != 0)
        return AXL_ERR;
    *bytes_read = got;
    return AXL_OK;
}

static void
dav_read_close(void *ctx)
{
    DavReadCtx *r = ctx;
    if (r == NULL) return;
    if (r->open) ft_close_read(&r->ctx);
    axl_free(r);
}

// ----------------------------------------------------------------------------
// Streaming write (PUT)
// ----------------------------------------------------------------------------

typedef struct {
    FtWriteCtx ctx;
    bool       open;
} DavWriteCtx;

static int
dav_write_open(void *user, const char *path, void **out_ctx)
{
    (void)user;
    *out_ctx = NULL;

    FtVolume vol;
    char     sub[512];
    if (parse_dav_path(path, &vol, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;

    DavWriteCtx *w = axl_calloc(1, sizeof(*w));
    if (w == NULL)
        return AXL_ERR;
    if (ft_open_write(&vol, sub, NULL, NULL, &w->ctx) != 0) {
        axl_free(w);
        return AXL_ERR;
    }
    w->open = true;
    *out_ctx = w;
    return AXL_OK;
}

static int
dav_write_chunk(void *ctx, const void *data, size_t len)
{
    DavWriteCtx *w = ctx;
    return ft_write_chunk(&w->ctx, data, len) == 0 ? AXL_OK : AXL_ERR;
}

static void
dav_write_close(void *ctx, bool aborted)
{
    (void)aborted;
    DavWriteCtx *w = ctx;
    if (w == NULL) return;
    if (w->open) ft_close_write(&w->ctx);
    axl_free(w);
}

// ----------------------------------------------------------------------------
// Lifecycle: MKCOL, DELETE, MOVE
// ----------------------------------------------------------------------------

static int
dav_mkdir(void *user, const char *path)
{
    (void)user;
    FtVolume vol;
    char     sub[512];
    if (parse_dav_path(path, &vol, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;
    return ft_mkdir(&vol, sub) == 0 ? AXL_OK : AXL_ERR;
}

static int
dav_remove(void *user, const char *path)
{
    (void)user;
    FtVolume vol;
    char     sub[512];
    if (parse_dav_path(path, &vol, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;

    /* WebDAV DELETE on a collection is well-defined; clients
       (Finder, davfs2) do issue it. Dispatch on is_dir so we route
       to rmdir / file delete appropriately. axl_dir_rmdir fails if
       the directory isn't empty -- RFC 4918 §9.6 leaves recursive
       delete semantics to the server; we refuse non-empty for now
       and let clients delete contents first. */
    bool is_dir = false;
    if (ft_is_dir(&vol, sub, &is_dir) != 0)
        return AXL_ERR;

    int rc = is_dir ? ft_rmdir(&vol, sub) : ft_delete(&vol, sub);
    return rc == 0 ? AXL_OK : AXL_ERR;
}

/* SHA-256 (RFC 3230 Want-Digest) — closes the WebDAV-side gap so
   mount clients pointing at /dav get the same end-to-end integrity
   validation the REST surface already provides. SDK c14abbc owns
   the Want-Digest parsing + response-header formatting; we just
   supply hex bytes. axl-webfs caches results in m_digest_cache (in
   webfs-serve.c) so the REST and /dav surfaces share one compute
   per (path, size). */
static int
dav_digest(void *user, const char *path, const char *algo,
           char *out_hex, size_t hex_size)
{
    (void)user;
    if (!axl_streql(algo, "sha-256") && !axl_streql(algo, "sha256"))
        return AXL_ERR;
    if (hex_size < 65) return AXL_ERR;

    FtVolume vol;
    char     sub[512];
    if (parse_dav_path(path, &vol, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;

    uint64_t size = 0;
    bool     is_dir = false;
    if (ft_is_dir(&vol, sub, &is_dir) != 0 || is_dir)
        return AXL_ERR;
    if (ft_get_file_size(&vol, sub, &size) != 0)
        return AXL_ERR;

    return compute_file_digest(&vol, sub, size, out_hex) == 0
           ? AXL_OK : AXL_ERR;
}

static int
dav_move(void *user, const char *src, const char *dst, bool overwrite)
{
    (void)user;
    (void)overwrite;  /* axl_file_rename's overwrite semantics are
                         backend-defined; FAT typically refuses. The
                         SDK maps AXL_ERR to 409 which clients retry. */

    FtVolume src_vol;
    FtVolume dst_vol;
    char     src_sub[512];
    char     dst_sub[512];

    if (parse_dav_path(src, &src_vol, src_sub, sizeof(src_sub))
            != PATH_OBJECT)
        return AXL_ERR;
    if (parse_dav_path(dst, &dst_vol, dst_sub, sizeof(dst_sub))
            != PATH_OBJECT)
        return AXL_ERR;

    return ft_move(&src_vol, src_sub, &dst_vol, dst_sub) == 0
           ? AXL_OK : AXL_ERR;
}

// ----------------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------------

static const AxlWebDavOps webfs_dav_ops = {
    .list_dir     = dav_list_dir,
    .stat         = dav_stat,
    .read_open    = dav_read_open,
    .read_chunk   = dav_read_chunk,
    .read_close   = dav_read_close,
    .write_open   = dav_write_open,
    .write_chunk  = dav_write_chunk,
    .write_close  = dav_write_close,
    .mkdir        = dav_mkdir,
    .remove       = dav_remove,
    .move         = dav_move,
    .digest       = dav_digest,
    /* copy left NULL — modern clients fall back to GET+PUT. */
};

int
webfs_dav_register(AxlHttpServer *server, const char *prefix, uint32_t auth_flags)
{
    return axl_http_server_add_webdav_auth(server, prefix, &webfs_dav_ops,
                                           NULL, auth_flags);
}
