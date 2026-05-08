/** @file
  axl-webfs -- serve core (shared between foreground app and DXE driver).

  Owns the URL parser, JSON-Accept sniffer, permission middleware, and
  every route handler — all the pieces both the foreground `serve` command
  and the resident webfs-server-dxe driver need. The only foreground-
  specific bits (ESC handler, banner, axl_loop_run) live in cmd-serve.c.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "serve/serve-core.h"

#include <axl.h>
#include <axl/axl-net.h>
#include <axl/axl-url.h>
#include "net/network.h"
#include "serve/upload-asset.h"
#include "transfer/file-transfer.h"


// ----------------------------------------------------------------------------
// URL parsing helpers
// ----------------------------------------------------------------------------

/// Parse "/<vol>/<path>" into volume name and sub-path.
/// Decodes percent-encoded characters in the URL path.
static int
parse_volume_path(const char *url_path, FtVolume *volume,
                  char *sub_path, size_t sub_path_size)
{
    // Decode percent-encoded path (e.g. %20 -> space)
    char decoded[512];
    axl_url_decode(url_path, decoded, sizeof(decoded));
    const char *p = decoded;
    char        vol_name[16];
    size_t      i;

    if (*p == '/') {
        p++;
    }

    i = 0;
    while (*p != '/' && *p != '\0' && i < 15) {
        vol_name[i++] = *p++;
    }

    vol_name[i] = '\0';

    if (ft_find_volume(vol_name, volume) != 0) {
        return -1;
    }

    i = 0;
    while (*p != '\0' && i < sub_path_size - 1) {
        sub_path[i++] = *p++;
    }

    sub_path[i] = '\0';

    if (sub_path[0] == '\0') {
        sub_path[0] = '/';
        sub_path[1] = '\0';
    }

    return 0;
}

/// Check if Accept header requests JSON.
static bool
wants_json(AxlHttpRequest *req)
{
    const char *accept = (const char *)axl_hash_table_lookup(req->headers, "accept");

    if (accept == NULL) {
        return false;
    }

    const char *p = accept;
    while (*p != '\0') {
        if (axl_strncmp(p, "application/json", 16) == 0) {
            return true;
        }

        p++;
    }

    return false;
}

// ----------------------------------------------------------------------------
// Permission middleware
// ----------------------------------------------------------------------------

static int
permission_middleware(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    ServeCoreOpts *opts = (ServeCoreOpts *)data;

    if (opts->read_only &&
        (axl_streql(req->method, "PUT") ||
         axl_streql(req->method, "DELETE") ||
         axl_streql(req->method, "POST")))
    {
        axl_http_response_set_text(resp, "Read-only mode\n");
        axl_http_response_set_status(resp, 403);
        return -1;
    }

    if (opts->write_only && axl_streql(req->method, "GET")) {
        axl_http_response_set_text(resp, "Write-only mode\n");
        axl_http_response_set_status(resp, 403);
        return -1;
    }

    return 0;
}

// ----------------------------------------------------------------------------
// Route handlers
// ----------------------------------------------------------------------------

/// GET /_axl-webfs/upload.js -- embedded upload UI script.
static int
handle_get_upload_js(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;

    axl_http_response_set_static(resp, upload_js, upload_js_len,
                                 "application/javascript");
    return 0;
}

/// GET / -- list all volumes.
static int
handle_get_root(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    size_t buf_size = 4096;
    char  *buf = axl_malloc(buf_size);
    size_t written = 0;
    bool   as_json = wants_json(req);

    (void)data;
    if (buf == NULL) {
        axl_http_response_set_status(resp, 500);
        return 0;
    }

    ft_list_volumes(as_json, buf, buf_size, &written);

    if (as_json) {
        axl_http_response_set_json(resp, buf);
        axl_free(buf);
    } else {
        resp->body = buf;
        resp->body_size = written;
        resp->content_type = "text/html";
        resp->status_code = 200;
    }

    return 0;
}

/// GET /<vol>/<path> -- download file or list directory.
static int
handle_get_path(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    ServeCoreOpts *opts = (ServeCoreOpts *)data;
    FtVolume       volume;
    char           sub_path[512];
    int            status;
    bool           is_dir;

    status = parse_volume_path(req->path, &volume, sub_path, 512);
    if (status != 0) {
        axl_http_response_set_text(resp, "Volume not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    is_dir = false;
    status = ft_is_dir(&volume, sub_path, &is_dir);
    if (status != 0) {
        axl_http_response_set_text(resp, "Not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    if (is_dir) {
        size_t buf_size = 16384;
        char  *buf = axl_malloc(buf_size);
        size_t written = 0;
        bool   as_json   = wants_json(req);
        bool   read_only = opts->read_only;

        if (buf == NULL) {
            axl_http_response_set_status(resp, 500);
            return 0;
        }

        status = ft_list_dir(&volume, sub_path, as_json, read_only,
                             buf, buf_size, &written);
        if (status != 0) {
            axl_free(buf);
            axl_http_response_set_text(resp, "Directory listing failed\n");
            axl_http_response_set_status(resp, 500);
            return 0;
        }

        if (as_json) {
            axl_http_response_set_json(resp, buf);
            axl_free(buf);
        } else {
            resp->body = buf;
            resp->body_size = written;
            resp->content_type = "text/html";
            resp->status_code = 200;
        }

        return 0;
    }

    //
    // File download -- read entire file into memory
    //
    uint64_t file_size = 0;
    status = ft_get_file_size(&volume, sub_path, &file_size);
    if (status != 0) {
        axl_http_response_set_text(resp, "File not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    FtReadCtx read_ctx;
    status = ft_open_read(&volume, sub_path, 0, NULL, NULL, &read_ctx);
    if (status != 0) {
        axl_http_response_set_text(resp, "Cannot open file\n");
        axl_http_response_set_status(resp, 500);
        return 0;
    }

    size_t full_size = (size_t)file_size;
    void *file_buf = axl_malloc(full_size);
    if (file_buf == NULL) {
        ft_close_read(&read_ctx);
        axl_http_response_set_text(resp, "Out of memory\n");
        axl_http_response_set_status(resp, 500);
        return 0;
    }

    size_t total_read = 0;
    while (total_read < full_size) {
        size_t got = 0;
        size_t want = full_size - total_read;
        if (want > FT_CHUNK_SIZE) {
            want = FT_CHUNK_SIZE;
        }

        status = ft_read_chunk(&read_ctx, (uint8_t *)file_buf + total_read,
                               want, &got);
        if (status != 0 || got == 0) {
            break;
        }

        total_read += got;
    }

    ft_close_read(&read_ctx);

    /* Handle Range header — set_range slices the buffer, sets 206 +
       Content-Range. */
    const char *range_hdr = (const char *)axl_hash_table_lookup(req->headers, "range");
    if (range_hdr != NULL) {
        AxlHttpRange range;
        if (axl_http_parse_range(range_hdr, file_size, &range)) {
            axl_http_response_set_range(resp, file_buf,
                (size_t)range.start,
                (size_t)(range.end - range.start + 1),
                total_read);
            axl_free(file_buf);
            return 0;
        }
    }

    resp->body = file_buf;
    resp->body_size = total_read;
    resp->content_type = "application/octet-stream";
    resp->status_code = 200;

    return 0;
}

/// PUT /<vol>/<path> -- upload/overwrite file.
static int
handle_put_path(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    FtVolume volume;
    char     sub_path[512];
    int      status;

    (void)data;

    status = parse_volume_path(req->path, &volume, sub_path, 512);
    if (status != 0) {
        axl_http_response_set_text(resp, "Volume not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    FtWriteCtx write_ctx;
    status = ft_open_write(&volume, sub_path, NULL, NULL, &write_ctx);
    if (status != 0) {
        axl_http_response_set_text(resp, "Cannot create file\n");
        axl_http_response_set_status(resp, 500);
        return 0;
    }

    if (req->body != NULL && req->body_size > 0) {
        size_t written = 0;
        while (written < req->body_size) {
            size_t chunk_size = req->body_size - written;
            if (chunk_size > FT_CHUNK_SIZE) {
                chunk_size = FT_CHUNK_SIZE;
            }

            status = ft_write_chunk(&write_ctx,
                         (const uint8_t *)req->body + written, chunk_size);
            if (status != 0) {
                break;
            }

            written += chunk_size;
        }
    }

    ft_close_write(&write_ctx);
    axl_http_response_set_text(resp, "Created\n");
    axl_http_response_set_status(resp, 201);
    return 0;
}

/// DELETE /<vol>/<path> -- delete file or empty directory.
static int
handle_delete_path(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    FtVolume volume;
    char     sub_path[512];
    int      status;

    (void)data;

    status = parse_volume_path(req->path, &volume, sub_path, 512);
    if (status != 0) {
        axl_http_response_set_text(resp, "Volume not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    status = ft_delete(&volume, sub_path);
    if (status != 0) {
        axl_http_response_set_text(resp, "Not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    axl_http_response_set_text(resp, "Deleted\n");
    return 0;
}

/// POST /<vol>/<path>?mkdir -- create directory.
static int
handle_post_path(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    FtVolume volume;
    char     sub_path[512];
    int      status;

    (void)data;

    if (req->query == NULL || !axl_streql(req->query, "mkdir")) {
        axl_http_response_set_text(resp, "Use ?mkdir\n");
        axl_http_response_set_status(resp, 400);
        return 0;
    }

    status = parse_volume_path(req->path, &volume, sub_path, 512);
    if (status != 0) {
        axl_http_response_set_text(resp, "Volume not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    status = ft_mkdir(&volume, sub_path);
    if (status != 0) {
        axl_http_response_set_text(resp, "mkdir failed\n");
        axl_http_response_set_status(resp, 500);
        return 0;
    }

    axl_http_response_set_text(resp, "Created\n");
    axl_http_response_set_status(resp, 201);
    return 0;
}

// ----------------------------------------------------------------------------
// Public bring-up / tear-down
// ----------------------------------------------------------------------------

int
serve_core_setup(const ServeCoreOpts *opts, ServeCore *core)
{
    axl_memset(core, 0, sizeof(*core));
    core->opts = *opts;

    if (network_init(opts->nic_index, NULL, 10) != 0) {
        return AXL_ERR;
    }

    network_get_address(core->addr);

    if (ft_init() != 0) {
        network_cleanup();
        return AXL_ERR;
    }

    core->server = axl_http_server_new(opts->port);
    if (core->server == NULL) {
        network_cleanup();
        return AXL_ERR;
    }

    /* Optional source-IP pin: empty / NULL = auto-pick. The HTTP server
       validates and returns an error if the value is malformed. */
    if (opts->source != NULL && opts->source[0] != '\0') {
        axl_http_server_set(core->server, "listen.ip", opts->source);
    }

    axl_http_server_set_body_limit(core->server, 128 * 1024 * 1024);  /* 128 MB */
    /* Single-threaded server -- keep-alive would block subsequent
       connections. */
    axl_http_server_set_keep_alive(core->server, 0);

    if (core->opts.read_only || core->opts.write_only) {
        axl_http_server_use(core->server, permission_middleware, &core->opts);
    }

    /* Routes. The SDK dispatcher tries exact matches before prefix
       patterns, so /_axl-webfs/upload.js shadows the GET wildcard
       regardless of registration order. The /_axl-webfs/ namespace is
       reserved for embedded UI assets so they don't collide with
       parse_volume_path's first-segment volume lookup. */
    axl_http_server_add_route(core->server, "GET",    "/_axl-webfs/upload.js",
                              handle_get_upload_js, NULL);
    axl_http_server_add_route(core->server, "GET",    "/",  handle_get_root,    NULL);
    axl_http_server_add_route(core->server, "GET",    "/*", handle_get_path,    &core->opts);
    axl_http_server_add_route(core->server, "PUT",    "/*", handle_put_path,    NULL);
    axl_http_server_add_route(core->server, "DELETE", "/*", handle_delete_path, NULL);
    axl_http_server_add_route(core->server, "POST",   "/*", handle_post_path,   NULL);

    core->loop = axl_loop_new();
    if (core->loop == NULL) {
        axl_http_server_free(core->server);
        core->server = NULL;
        network_cleanup();
        return AXL_ERR;
    }

    if (axl_http_server_attach(core->server, core->loop) != 0) {
        axl_loop_free(core->loop);
        core->loop = NULL;
        axl_http_server_free(core->server);
        core->server = NULL;
        network_cleanup();
        return AXL_ERR;
    }

    return AXL_OK;
}

void
serve_core_teardown(ServeCore *core)
{
    if (core == NULL) {
        return;
    }

    /* Free the server FIRST -- its detach path removes TCP listener
       event sources from the loop. Reversing this order leaves
       caller-owned event sources active when axl_loop_free walks the
       table and triggers a use-after-free warning (and a real crash on
       the next loop ops). */
    if (core->server != NULL) {
        axl_http_server_free(core->server);
        core->server = NULL;
    }

    if (core->loop != NULL) {
        axl_loop_free(core->loop);
        core->loop = NULL;
    }

    network_cleanup();
}
