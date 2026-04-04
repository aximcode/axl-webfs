/** @file
  HttpFS -- serve command handler.

  HTTP file server using AxlNet's AxlHttpServer. Registers route
  handlers that map REST API endpoints to FileTransferLib operations.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "httpfs-internal.h"

#include <axl.h>
#include <axl/axl-net.h>
#include <axl/axl-url.h>
#include "net/network.h"
#include "transfer/file-transfer.h"



// ----------------------------------------------------------------------------
// Options (passed as handler Data via route registration)
// ----------------------------------------------------------------------------

typedef struct {
    uint16_t port;
    size_t   nic_index;
    size_t   idle_timeout_sec;
    bool     read_only;
    bool     write_only;
    bool     verbose;
} ServeOptions;

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
    const char *accept = (const char *)axl_hash_table_get(req->headers, "accept");

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
    ServeOptions *opts = (ServeOptions *)data;

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
    FtVolume  volume;
    char       sub_path[512];
    int        status;
    bool       is_dir;

    (void)data;

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
        size_t buf_size = 8192;
        char  *buf = axl_malloc(buf_size);
        size_t written = 0;
        bool   as_json = wants_json(req);

        if (buf == NULL) {
            axl_http_response_set_status(resp, 500);
            return 0;
        }

        status = ft_list_dir(&volume, sub_path, as_json, buf, buf_size, &written);
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

    //
    // Read entire file into memory
    //
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

    //
    // Handle Range header — set_range slices the buffer, sets 206 + Content-Range
    //
    const char *range_hdr = (const char *)axl_hash_table_get(req->headers, "range");
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
    char      sub_path[512];
    int       status;

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

    //
    // Write request body (already fully read by AxlHttpServer)
    //
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
    char      sub_path[512];
    int       status;

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
    char      sub_path[512];
    int       status;

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
// ESC key handler for the event loop
// ----------------------------------------------------------------------------

static AxlLoop *mServeLoop = NULL;

static bool
esc_key_handler(AxlInputKey key, void *data)
{
    (void)data;

    if (key.scan_code == 0x17) {  /* SCAN_ESC */
        axl_printf("\nESC -- stopping server.\n");
        if (mServeLoop != NULL) {
            axl_loop_quit(mServeLoop);
        }

        return false;
    }

    return true;
}

// ----------------------------------------------------------------------------
// Main serve command
// ----------------------------------------------------------------------------

int
cmd_serve(int argc, char **argv)
{
    ServeOptions    opts;
    AxlHttpServer  *server;

    static const AxlConfigDesc serve_descs[] = {
        { "port",       AXL_CFG_UINT, "8080",  'p', "Listen port",        0, 0 },
        { "nic",        AXL_CFG_UINT, NULL,     'n', "NIC index",          0, 0 },
        { "timeout",    AXL_CFG_UINT, "0",      't', "Idle timeout (sec)", 0, 0 },
        { "read-only",  AXL_CFG_BOOL, "false",   0,  "Block uploads",     0, 0 },
        { "write-only", AXL_CFG_BOOL, "false",   0,  "Block downloads",   0, 0 },
        { "verbose",    AXL_CFG_BOOL, "false",  'v', "Verbose logging",   0, 0 },
        { "help",       AXL_CFG_BOOL, "false",  'h', "Show help",         0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(serve_descs, NULL, NULL);
    if (cfg == NULL) return 1;
    axl_config_parse_args(cfg, argc, argv);

    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "HttpFS serve", "[OPTIONS]");
        axl_config_free(cfg);
        return 0;
    }

    opts.port             = (uint16_t)axl_config_get_uint(cfg, "port");
    opts.nic_index        = axl_config_get(cfg, "nic") != NULL
                          ? (size_t)axl_config_get_uint(cfg, "nic") : (size_t)-1;
    opts.idle_timeout_sec = (size_t)axl_config_get_uint(cfg, "timeout");
    opts.read_only        = axl_config_get_bool(cfg, "read-only");
    opts.write_only       = axl_config_get_bool(cfg, "write-only");
    opts.verbose          = axl_config_get_bool(cfg, "verbose");

    axl_config_free(cfg);

    //
    // Initialize networking (driver loading, DHCP)
    //
    if (network_init(opts.nic_index, NULL, 10) != 0) {
        axl_printf("ERROR: Network init failed\n");
        return 1;
    }

    uint8_t addr[4];
    network_get_address(addr);

    //
    // Initialize file transfer
    //
    if (ft_init() != 0) {
        axl_printf("ERROR: FileTransfer init failed\n");
        network_cleanup();
        return 1;
    }

    //
    // Create HTTP server
    //
    server = axl_http_server_new(opts.port);
    if (server == NULL) {
        axl_printf("ERROR: HTTP server creation failed\n");
        network_cleanup();
        return 1;
    }

    axl_http_server_set_body_limit(server, 128 * 1024 * 1024);  // 128 MB

    //
    // Disable keep-alive (avoids blocking on single-threaded server)
    //
    axl_http_server_set_keep_alive(server, 0);

    //
    // Permission middleware
    //
    if (opts.read_only || opts.write_only) {
        axl_http_server_use(server, permission_middleware, &opts);
    }

    //
    // Register routes (exact root match first, then prefix matches)
    //
    axl_http_server_add_route(server, "GET",    "/",  handle_get_root,    NULL);
    axl_http_server_add_route(server, "GET",    "/*", handle_get_path,    NULL);
    axl_http_server_add_route(server, "PUT",    "/*", handle_put_path,    NULL);
    axl_http_server_add_route(server, "DELETE", "/*", handle_delete_path, NULL);
    axl_http_server_add_route(server, "POST",   "/*", handle_post_path,   NULL);

    //
    // Print banner
    //
    axl_printf("\nHttpFS v0.1 -- UEFI HTTP File Server\n");
    axl_printf("Listening on %d.%d.%d.%d:%d\n",
        addr[0], addr[1], addr[2], addr[3], opts.port);

    const char *mode = "read-write";
    if (opts.read_only) {
        mode = "read-only";
    }

    if (opts.write_only) {
        mode = "write-only";
    }

    axl_printf("Mode: %s\n", mode);

    axl_printf("Volumes:\n");
    size_t vcount = ft_get_volume_count();
    for (size_t i = 0; i < vcount; i++) {
        FtVolume vol;
        ft_get_volume(i, &vol);
        axl_printf("  %s:\n", vol.name);
    }

    axl_printf("Press ESC to stop.\n\n");

    //
    // Run server with event loop (ESC to quit)
    //
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        axl_http_server_free(server);
        network_cleanup();
        return 1;
    }

    mServeLoop = loop;
    axl_loop_add_key_press(loop, esc_key_handler, NULL);

    if (axl_http_server_attach(server, loop) != 0) {
        axl_printf("ERROR: Server attach failed\n");
        axl_loop_free(loop);
        axl_http_server_free(server);
        network_cleanup();
        return 1;
    }

    int status = axl_loop_run(loop);

    mServeLoop = NULL;
    axl_loop_free(loop);
    axl_http_server_free(server);
    network_cleanup();
    return status;
}
