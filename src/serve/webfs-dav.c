/** @file
  axl-webfs -- WebDAV class-1 + MOVE adapter: a virtual root of UEFI
  volumes over the SDK's own filesystem WebDAV backend.

  All protocol bits (verb dispatch, PROPFIND XML, Depth/Destination/
  Overwrite parsing, DAV: 1 advertisement) live in axl-sdk.

  URL layout: /dav/<vol>/<path>. Clients (Finder, Explorer, davfs2,
  cadaver) see the mount root /dav as a collection containing one
  virtual subdirectory per UEFI volume -- list_dir("/") returns the
  volume names; everything under /dav/<vol>/ maps to that volume's
  filesystem.

  Every op that touches ONE path below a volume -- PROPFIND stat,
  GET/HEAD, PUT, MKCOL, DELETE -- is forwarded to the SDK's
  axl_fs_webdav_ops() table over one AxlFsRoot per volume. That is what
  makes a PUT atomic: the SDK writes the body to a temp sibling
  ("~APxxxxx.TMP"), replaces the target with axl_file_replace only on
  the commit (write_close(false)), and on a discard (write_close(true):
  a client hang-up, a failed chunk, a Content-Digest mismatch) deletes
  the temp through the writer's own handle, leaving the old file
  exactly as it was. The same table answers a request naming a live
  temp with "not found"; the PROPFIND and REST listings leave it out
  through dav_live_temp. Deleting
  through the handle, and flushing with it still open, are SDK-internal,
  so this cannot be rebuilt on the public AxlFileWriter: forwarding is
  how this mount keeps that guarantee.

  What stays here: the virtual root and volume entries, directory
  listing (see dav_list_dir), MOVE (it
  crosses volumes, which no single AxlFsRoot can do), and Want-Digest
  (it shares webfs-serve.c's digest cache). caps stays 0: MOVE does
  not decide identity by what the volume stores, so a case-variant
  `Overwrite: F` transfer is refused 412 by the SDK, never forwarded.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "serve/webfs-dav.h"
#include "transfer/file-transfer.h"

#include <axl.h>

AXL_LOG_DOMAIN("webfs-dav");

/* Length of the SDK's PUT temp name, "~APxxxxx.TMP" (axl-http-server.h,
   axl_fs_webdav_ops). */
#define DAV_TEMP_NAME_LEN  12

/* Forward decls into webfs-serve.c -- both files are compiled into the
   serve DXE driver, so the linker resolves these without a header.
   Sharing keeps the SHA-256 cache (m_digest_cache) in one place. */
extern int  compute_file_digest(FtVolume *volume, const char *sub_path,
                                uint64_t file_size, char *out_hex);
extern void invalidate_file_digest(const FtVolume *volume,
                                   const char *sub_path);

typedef enum {
    PATH_ROOT,
    PATH_VOLUME,
    PATH_OBJECT,
    PATH_INVALID,           ///< unknown volume, or a path that does not fit
} DavPathKind;

/* A PUT in flight (REST or /dav): the SDK's write ctx, plus where it
   lands, so the commit can drop that path's cached digest. */
typedef struct {
    void     *inner;
    FtVolume  vol;
    char      sub[512];
} WebfsPut;

/* One AxlFsRoot ("<vol>:") per volume ft_init found, index-parallel to
   ft_get_volume(). Created by webfs_dav_register, freed by
   webfs_dav_unregister. */
static AxlFsRoot          *m_roots[FT_MAX_VOLUMES];

/* The SDK's per-subtree table the single-path ops are forwarded to. */
static const AxlWebDavOps *m_fs;

/* `serve -v`: name each PUT's temp in the log. */
static bool                m_verbose;

// ----------------------------------------------------------------------------
// Path parsing
// ----------------------------------------------------------------------------

/* Parse a WebDAV-relative path "/<vol>/<sub>" (or "/" / "/<vol>"
   variants). Returns one of three categories so callers know whether
   to treat the path as the virtual root, a volume root, or a real
   filesystem object.

   Layout summary:
     PATH_ROOT     "/"           vol=N/A  sub=N/A   root=N/A
     PATH_VOLUME   "/<vol>"      vol=set  sub="/"   root=set
     PATH_OBJECT   "/<vol>/x"    vol=set  sub="/x"  root=set

   The volume root case (PATH_VOLUME) is distinguished from
   PATH_OBJECT with sub="/" because a missing volume should map to
   404 even when sub looks like a valid root path. A volume whose
   AxlFsRoot could not be allocated is PATH_INVALID. */
static DavPathKind
parse_dav_path(const char *url, FtVolume *vol, AxlFsRoot **root,
               char *sub, size_t sub_size)
{
    if (url == NULL || url[0] == '\0' ||
        (url[0] == '/' && url[1] == '\0')) {
        return PATH_ROOT;
    }

    const char *p = url;
    if (*p == '/')
        p++;

    /* A volume name too long to be one is no volume -- never a cut
       one that might match. */
    const char *vol_end = p;
    while (*vol_end != '/' && *vol_end != '\0')
        vol_end++;
    char vol_name[16];
    if ((size_t)(vol_end - p) >= sizeof(vol_name))
        return PATH_INVALID;
    axl_memcpy(vol_name, p, (size_t)(vol_end - p));
    vol_name[vol_end - p] = '\0';
    p = vol_end;

    *root = NULL;
    size_t n = ft_get_volume_count();
    for (size_t j = 0; j < n && j < FT_MAX_VOLUMES; j++) {
        if (ft_get_volume(j, vol) == 0 && axl_streql(vol->name, vol_name)) {
            *root = m_roots[j];
            break;
        }
    }
    if (*root == NULL)
        return PATH_INVALID;

    if (*p == '\0') {
        axl_strlcpy(sub, "/", sub_size);
        return PATH_VOLUME;
    }

    /* p now points at '/' starting the sub-path. Refused, never cut,
       when it -- or the "<vol>:<sub>" path ft_* build from it -- does
       not fit: a cut path names another file. */
    char full[FT_PATH_MAX];
    if (axl_strlcpy(sub, p, sub_size) >= sub_size ||
        ft_build_path(vol, sub, full, sizeof(full)) != 0)
        return PATH_INVALID;
    return PATH_OBJECT;
}

/* Open volume-relative directory @p sub ("/" or "/a/b") on @p vol. */
static AxlDir *
dav_dir_open(const FtVolume *vol, const char *sub)
{
    char full[FT_PATH_MAX];
    if (ft_build_path(vol, sub, full, sizeof(full)) != 0)
        return NULL;
    return axl_dir_open(full);
}

/* Whether @p name has the shape of the SDK's PUT temp, "~APxxxxx.TMP"
   (five hex digits), in any letter case. */
static bool
dav_temp_shaped(const char *name)
{
    if (axl_strlen(name) != DAV_TEMP_NAME_LEN ||
        axl_strncasecmp(name, "~AP", 3) != 0 ||
        axl_strcasecmp(name + 8, ".TMP") != 0) {
        return false;
    }
    for (size_t i = 3; i < 8; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* Whether @p sub on @p root names an in-flight PUT's temp. The SDK
   keeps the registry of live temps private, but its stat refuses
   exactly those, so a temp-shaped name that exists while that stat
   fails is one. A temp a power loss left behind stats normally and is
   an ordinary file, as the SDK documents. */
static bool
dav_live_temp(AxlFsRoot *root, const FtVolume *vol, const char *sub)
{
    const char *base = sub;
    for (const char *q = sub; *q != '\0'; q++)
        if (*q == '/' && q[1] != '\0')
            base = q + 1;
    if (!dav_temp_shaped(base))
        return false;

    AxlFsEntry e;
    if (m_fs->stat(root, sub, &e) == AXL_OK)
        return false;
    return ft_stat((FtVolume *)vol, sub, &e) == 0;
}

/* Whether entry @p name, listed in volume-relative directory @p dir
   ("/" or "/a/b[/]"), is a live PUT temp a listing must leave out. */
static bool
dav_hidden_child(AxlFsRoot *root, const FtVolume *vol, const char *dir,
                 const char *name)
{
    if (!dav_temp_shaped(name))
        return false;
    char   child[512];
    size_t dlen  = axl_strlen(dir);
    bool   slash = dlen > 0 && dir[dlen - 1] == '/';
    axl_snprintf(child, sizeof(child), "%s%s%s", dir, slash ? "" : "/", name);
    return dav_live_temp(root, vol, child);
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

    FtVolume   vol;
    AxlFsRoot *root;
    char       sub[512];
    DavPathKind kind = parse_dav_path(path, &vol, &root, sub, sizeof(sub));

    if (kind == PATH_INVALID)
        return AXL_ERR;

    /* Mount root -> one virtual entry per UEFI volume. */
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

    /* A real directory on a volume (incl. the volume root, sub="/").
       Listed here, not by the SDK op: an AxlFsRoot strips the root's
       trailing separator, so for a bare volume ("fs0:") its list_dir
       opens "fs0:" -- which lists nothing -- rather than "fs0:\".
       dav_live_temp keeps an in-flight PUT's temp out, as that op
       would. TODO: forward to m_fs->list_dir once the SDK's
       axl_fs_root_new keeps a bare volume's separator (being fixed in
       axl-sdk). */
    AxlDir *dir = dav_dir_open(&vol, sub);
    if (dir == NULL)
        return AXL_ERR;

    AxlFsEntry e;
    size_t     n = 0;
    while (n < max && axl_dir_read(dir, &e)) {
        if (axl_streql(e.name, ".") || axl_streql(e.name, ".."))
            continue;
        if (dav_hidden_child(root, &vol, sub, e.name))
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

    FtVolume   vol;
    AxlFsRoot *root;
    char       sub[512];
    DavPathKind kind = parse_dav_path(path, &vol, &root, sub, sizeof(sub));

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
            int rc = m_fs->stat(root, sub, out);
            if (rc != AXL_OK)
                return rc;
            /* basename for name field. */
            const char *base = sub;
            for (const char *q = sub; *q; q++)
                if (*q == '/' && q[1] != '\0')
                    base = q + 1;
            axl_strlcpy(out->name, base, sizeof(out->name));
            if (axl_fs_entry_is_dir(out))
                out->size = 0;
            return AXL_OK;
        }
        case PATH_INVALID:
        default:
            return AXL_ERR;
    }
}

// ----------------------------------------------------------------------------
// Streaming read (GET / HEAD) -- read_chunk / read_close are the SDK's
// ----------------------------------------------------------------------------

static int
dav_read_open(void *user, const char *path, uint64_t offset, void **out_ctx)
{
    (void)user;
    *out_ctx = NULL;

    FtVolume   vol;
    AxlFsRoot *root;
    char       sub[512];
    if (parse_dav_path(path, &vol, &root, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;
    return m_fs->read_open(root, sub, offset, out_ctx);
}

// ----------------------------------------------------------------------------
// Streaming write (PUT) -- atomic, by the SDK's temp + replace
// ----------------------------------------------------------------------------

/* The AxlFsRoot of @p vol (by name), or NULL. */
static AxlFsRoot *
dav_root_for(const FtVolume *vol)
{
    size_t n = ft_get_volume_count();
    for (size_t j = 0; j < n && j < FT_MAX_VOLUMES; j++) {
        FtVolume v;
        if (ft_get_volume(j, &v) == 0 && axl_streql(v.name, vol->name))
            return m_roots[j];
    }
    return NULL;
}

bool
webfs_live_temp(const FtVolume *vol, const char *sub)
{
    AxlFsRoot *root = dav_root_for(vol);
    return root != NULL && dav_live_temp(root, vol, sub);
}

/* The live temps in volume-relative directory @p dir, into @p names
   (up to @p max); returns how many. For the `-v` log only: a scan per
   PUT is not paid otherwise. */
static size_t
dav_live_temps_in(AxlFsRoot *root, const FtVolume *vol, const char *dir,
                  char names[][DAV_TEMP_NAME_LEN + 1], size_t max)
{
    AxlDir *d = dav_dir_open(vol, dir);
    if (d == NULL)
        return 0;
    size_t     n = 0;
    AxlFsEntry e;
    while (n < max && axl_dir_read(d, &e)) {
        if (dav_hidden_child(root, vol, dir, e.name))
            axl_strlcpy(names[n++], e.name, DAV_TEMP_NAME_LEN + 1);
    }
    axl_dir_close(d);
    return n;
}

/* Log the temp @p w's PUT streams to: the live temp in its directory
   that was not live before the open (@p before, @p n_before). */
static void
dav_log_put_temp(AxlFsRoot *root, const WebfsPut *w, const char *dir,
                 char before[][DAV_TEMP_NAME_LEN + 1], size_t n_before)
{
    char   now[8][DAV_TEMP_NAME_LEN + 1];
    size_t n_now = dav_live_temps_in(root, &w->vol, dir, now, 8);
    for (size_t i = 0; i < n_now; i++) {
        bool seen = false;
        for (size_t j = 0; j < n_before && !seen; j++)
            seen = axl_strcasecmp(now[i], before[j]) == 0;
        if (!seen) {
            axl_info("PUT %s:%s: body streams to temp %s", w->vol.name,
                     w->sub, now[i]);
            return;
        }
    }
}

int
webfs_put_open(const FtVolume *vol, const char *sub, void **out_ctx)
{
    *out_ctx = NULL;
    AxlFsRoot *root = dav_root_for(vol);
    if (root == NULL)
        return AXL_ERR;
    if (sub[0] == '\0' || (sub[0] == '/' && sub[1] == '\0'))
        return AXL_IS_DIR;          /* the volume root is a collection */

    WebfsPut *w = axl_calloc(1, sizeof(*w));
    if (w == NULL)
        return AXL_NO_RESOURCES;
    w->vol = *vol;
    axl_strlcpy(w->sub, sub, sizeof(w->sub));

    /* dirname("/a.txt") is "", which as a directory is the volume
       root, "/". */
    char  *dir = m_verbose ? axl_path_get_dirname(sub) : NULL;
    if (dir != NULL && dir[0] == '\0') {
        axl_free(dir);
        dir = axl_strdup("/");
    }
    char   before[8][DAV_TEMP_NAME_LEN + 1];
    size_t n_before = dir != NULL
                      ? dav_live_temps_in(root, vol, dir, before, 8) : 0;

    int rc = m_fs->write_open(root, sub, &w->inner);
    if (rc != AXL_OK) {
        axl_free(dir);
        axl_free(w);
        return rc;
    }
    if (dir != NULL)
        dav_log_put_temp(root, w, dir, before, n_before);
    axl_free(dir);
    *out_ctx = w;
    return AXL_OK;
}

int
webfs_put_write(void *ctx, const void *data, size_t len)
{
    WebfsPut *w = ctx;
    return m_fs->write_chunk(w->inner, data, len);
}

/* discard=false COMMITS: the SDK flushes the temp and replaces the
   target, and its status is the PUT's. discard=true DISCARDS: the SDK
   deletes the temp through its handle and the target is left as it was.
   Either way the ctx is dead afterwards. A commit changes the path's
   bytes, so its cached digest goes with it -- the cache is keyed by
   size, which a same-size replace does not change. */
int
webfs_put_close(void *ctx, bool discard)
{
    WebfsPut *w = ctx;
    if (w == NULL)
        return AXL_ERR;
    int rc = m_fs->write_close(w->inner, discard);
    if (!discard && rc == AXL_OK)
        invalidate_file_digest(&w->vol, w->sub);
    axl_free(w);
    return rc;
}

static int
dav_write_open(void *user, const char *path, void **out_ctx)
{
    (void)user;
    *out_ctx = NULL;

    FtVolume   vol;
    AxlFsRoot *root;
    char       sub[512];
    switch (parse_dav_path(path, &vol, &root, sub, sizeof(sub))) {
        case PATH_OBJECT:
            return webfs_put_open(&vol, sub, out_ctx);
        case PATH_VOLUME:
            return AXL_IS_DIR;      /* 409: a collection at the target */
        default:
            return AXL_ERR;         /* 409: no such parent collection */
    }
}

// ----------------------------------------------------------------------------
// Lifecycle: MKCOL, DELETE, MOVE
// ----------------------------------------------------------------------------

static int
dav_mkdir(void *user, const char *path)
{
    (void)user;
    FtVolume   vol;
    AxlFsRoot *root;
    char       sub[512];
    if (parse_dav_path(path, &vol, &root, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;
    return m_fs->mkdir(root, sub);
}

/* WebDAV DELETE on a collection is well-defined; clients (Finder,
   davfs2) do issue it. The SDK op removes a file, or an EMPTY
   directory -- RFC 4918 §9.6 leaves recursive delete to the server,
   and this one refuses non-empty, so clients delete contents first. */
static int
dav_remove(void *user, const char *path)
{
    (void)user;
    FtVolume   vol;
    AxlFsRoot *root;
    char       sub[512];
    if (parse_dav_path(path, &vol, &root, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;
    int rc = m_fs->remove(root, sub);
    if (rc == AXL_OK)
        invalidate_file_digest(&vol, sub);
    return rc;
}

/* SHA-256 (RFC 3230 Want-Digest) -- closes the WebDAV-side gap so
   mount clients pointing at /dav get the same end-to-end integrity
   validation the REST surface already provides. SDK c14abbc owns
   the Want-Digest parsing + response-header formatting; we just
   supply hex bytes. axl-webfs caches results in m_digest_cache (in
   webfs-serve.c) so the REST and /dav surfaces share one compute
   per (path, size). A live PUT temp is no resource, so it has none. */
static int
dav_digest(void *user, const char *path, const char *algo,
           char *out_hex, size_t hex_size)
{
    (void)user;
    if (!axl_streql(algo, "sha-256") && !axl_streql(algo, "sha256"))
        return AXL_ERR;
    if (hex_size < 65) return AXL_ERR;

    FtVolume   vol;
    AxlFsRoot *root;
    char       sub[512];
    if (parse_dav_path(path, &vol, &root, sub, sizeof(sub)) != PATH_OBJECT)
        return AXL_ERR;
    if (dav_live_temp(root, &vol, sub))
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

/* MOVE crosses volumes (/dav/fs0/a -> /dav/fs1/b), so it is ft_move
   (axl_file_move), not the per-volume SDK op. The SDK has already
   answered a missing source (404, through stat -- which also hides a
   live PUT temp) and an existing destination under `Overwrite: F`
   (412); with `Overwrite: T` axl_file_move replaces the destination.
   A live PUT temp is refused as a destination too (403): replacing it
   would pull the file out from under the PUT. axl_file_move's code is
   returned as is, so the SDK maps it (403/409/412/502/507/500). */
static int
dav_move(void *user, const char *src, const char *dst, bool overwrite)
{
    (void)user;
    (void)overwrite;  /* see above: decided by the SDK before this call */

    FtVolume   src_vol;
    FtVolume   dst_vol;
    AxlFsRoot *src_root;
    AxlFsRoot *dst_root;
    char       src_sub[512];
    char       dst_sub[512];

    if (parse_dav_path(src, &src_vol, &src_root, src_sub, sizeof(src_sub))
            != PATH_OBJECT)
        return AXL_ERR;
    if (parse_dav_path(dst, &dst_vol, &dst_root, dst_sub, sizeof(dst_sub))
            != PATH_OBJECT)
        return AXL_ERR;
    if (dav_live_temp(src_root, &src_vol, src_sub))
        return AXL_NOT_FOUND;
    if (dav_live_temp(dst_root, &dst_vol, dst_sub))
        return AXL_DENIED;

    int rc = ft_move(&src_vol, src_sub, &dst_vol, dst_sub);
    if (rc == AXL_OK) {
        invalidate_file_digest(&src_vol, src_sub);
        invalidate_file_digest(&dst_vol, dst_sub);
    }
    return rc;
}

/* ft_list_dir filter: the REST listing hides a live PUT temp the same
   way the PROPFIND listing does. @p dir is the listed directory's
   volume-relative path ("/" or "/a/b[/]"). */
static bool
dav_hide_entry(const FtVolume *vol, const char *dir, const char *name)
{
    AxlFsRoot *root = dav_root_for(vol);
    return root != NULL && dav_hidden_child(root, vol, dir, name);
}

// ----------------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------------

int
webfs_dav_register(AxlHttpServer *server, const char *prefix,
                   uint32_t auth_flags, bool verbose)
{
    m_fs      = axl_fs_webdav_ops();
    m_verbose = verbose;

    size_t n = ft_get_volume_count();
    for (size_t j = 0; j < n && j < FT_MAX_VOLUMES; j++) {
        FtVolume v;
        char     base[24];
        if (ft_get_volume(j, &v) != 0)
            continue;
        axl_snprintf(base, sizeof(base), "%s:", v.name);
        m_roots[j] = axl_fs_root_new(base, 0);
        if (m_roots[j] == NULL)
            axl_warning("no WebDAV root for volume %s", v.name);
    }
    ft_set_list_filter(dav_hide_entry);

    /* caps stays 0 -- see the file header. copy stays NULL: modern
       clients fall back to GET+PUT. */
    AxlWebDavOps ops = {
        .list_dir     = dav_list_dir,
        .stat         = dav_stat,
        .read_open    = dav_read_open,
        .read_chunk   = m_fs->read_chunk,
        .read_close   = m_fs->read_close,
        .write_open   = dav_write_open,
        .write_chunk  = webfs_put_write,
        .write_close  = webfs_put_close,
        .mkdir        = dav_mkdir,
        .remove       = dav_remove,
        .move         = dav_move,
        .digest       = dav_digest,
    };
    return axl_http_server_add_webdav_auth(server, prefix, &ops,
                                           NULL, auth_flags);
}

void
webfs_dav_unregister(void)
{
    ft_set_list_filter(NULL);
    for (size_t j = 0; j < FT_MAX_VOLUMES; j++) {
        axl_fs_root_free(m_roots[j]);
        m_roots[j] = NULL;
    }
}
