/** @file
  axl-webfs -- serve service.

  Single-source-file dual-compile. With -DAXL_SERVICE_BUILD_DRIVER
  this builds into webfs-serve-dxe.efi and includes setup/teardown,
  the route handlers, and AXL_SERVICE_DRIVER's DriverEntry. Without
  the define only the unconditional bits compile (g_serve_opts,
  serve_descs, the webfs_serve descriptor stub), which is what the
  launcher needs for axl_service_start_embedded's LoadOptions
  serialization.

  Cross-binary ABI rule: same source, same flags except for the
  AXL_SERVICE_BUILD_DRIVER toggle. AxlConfig auto-apply over
  LoadOptions is safe under that rule.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "serve/webfs-serve.h"

#include <axl.h>

#ifdef AXL_SERVICE_BUILD_DRIVER
#  include <axl/axl-net.h>
#  include <axl/axl-pubsub.h>
#  include <axl/axl-url.h>
#  include "net/network.h"
#  include "serve/upload-asset.h"
#  include "transfer/file-transfer.h"

/* Pub/sub topic for non-GET request notifications. The default
   subscriber is the in-process console printer registered in
   serve_setup; external consumers can subscribe to the same topic
   for telemetry / audit. */
#  define WEBFS_REQUEST_TOPIC "webfs.request"

/* Single-buffer event payload reused across requests. Safe because
   UEFI is single-threaded and pubsub deferred dispatch drains before
   the next request handler runs. Strings are inline copies so the
   underlying req fields can be freed when the handler returns. */
typedef struct {
    char     method[16];
    char     path[256];
    uint64_t status;
    uint64_t body_size;
} WebfsRequestEvent;

static WebfsRequestEvent m_req_event;
#endif /* AXL_SERVICE_BUILD_DRIVER */

ServeOpts g_serve_opts;

const AxlConfigDesc serve_descs[] = {
    { "port",     AXL_CFG_UINT,   "8080",       "Listen port",
      offsetof(ServeOpts, port),             sizeof(uint64_t) },
    { "nic",      AXL_CFG_UINT,   "18446744073709551615", /* (uint64_t)-1 = auto */
                                              "NIC index (auto if unset)",
      offsetof(ServeOpts, nic_index),        sizeof(uint64_t) },
    { "timeout",  AXL_CFG_UINT,   "0",          "Idle timeout in seconds (0 = none)",
      offsetof(ServeOpts, idle_timeout_sec), sizeof(uint64_t) },
    { "verbose",  AXL_CFG_BOOL,   "false",      "Verbose logging",
      offsetof(ServeOpts, verbose),          sizeof(bool) },
    { "mode",     AXL_CFG_STRING, "read-write", "Permission mode",
      offsetof(ServeOpts, mode),             sizeof(const char *) },
    { "source",   AXL_CFG_STRING, "",           "Bind to interface with this IPv4 (auto if empty)",
      offsetof(ServeOpts, source),           sizeof(const char *) },
    { "log",      AXL_CFG_STRING, "",           "Log file path (empty = console only)",
      offsetof(ServeOpts, log_path),         sizeof(const char *) },
    { 0 }
};

#ifdef AXL_SERVICE_BUILD_DRIVER

AXL_LOG_DOMAIN("webfs-serve");

// ----------------------------------------------------------------------------
// URL parsing helpers
// ----------------------------------------------------------------------------

/// Parse "/<vol>/<path>" into volume name and sub-path.
/// Decodes percent-encoded characters in the URL path.
static int
parse_volume_path(const char *url_path, FtVolume *volume,
                  char *sub_path, size_t sub_path_size)
{
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

/// Does the client EXPLICITLY ask for JSON? The SDK's
/// axl_http_request_wants_json wraps axl_http_accepts which honors
/// the */* wildcard -- correct content negotiation per RFC 9110, but
/// wrong for a UEFI tool where curl's default Accept: */* should get
/// the human-readable HTML, not JSON. Stick to a literal substring
/// scan so JSON is opt-in via an explicit application/json mention.
static bool
wants_json(AxlHttpRequest *req)
{
    const char *accept = (const char *)axl_hash_table_lookup(req->headers, "accept");
    if (accept == NULL) {
        return false;
    }
    for (const char *p = accept; *p != '\0'; p++) {
        if (axl_strncmp(p, "application/json", 16) == 0) {
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// Request-event publish helper
// ----------------------------------------------------------------------------

/// Snapshot the request method/path + the response status/body size into
/// the file-static event buffer and publish on WEBFS_REQUEST_TOPIC.
/// Subscribers (the console printer is the default) get a deferred
/// callback at the next loop tick.
static void
publish_request(AxlLoop *loop, AxlHttpRequest *req, AxlHttpResponse *resp)
{
    if (loop == NULL) {
        return;
    }

    axl_strlcpy(m_req_event.method, req->method != NULL ? req->method : "?",
                sizeof(m_req_event.method));
    axl_strlcpy(m_req_event.path,   req->path   != NULL ? req->path   : "?",
                sizeof(m_req_event.path));
    m_req_event.status    = (uint64_t)resp->status_code;
    m_req_event.body_size = (uint64_t)req->body_size;

    (void)axl_pubsub_publish(loop, WEBFS_REQUEST_TOPIC, &m_req_event);
}

/// Default console-feedback subscriber. Prints one line per non-GET
/// request so a shell user knows when remote clients are mutating the
/// filesystem. Registered in serve_setup, unregistered in
/// serve_teardown.
static void
on_webfs_request(void *event_data, void *user_data)
{
    (void)user_data;
    WebfsRequestEvent *e = (WebfsRequestEvent *)event_data;
    if (e == NULL) {
        return;
    }

    axl_info("%s %s -> %llu (%llu bytes)",
             e->method, e->path,
             (unsigned long long)e->status,
             (unsigned long long)e->body_size);
}

// ----------------------------------------------------------------------------
// Permission middleware
// ----------------------------------------------------------------------------

static int
permission_middleware(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    ServeOpts *opts = (ServeOpts *)data;

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

static int
handle_get_upload_js(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_static(resp, upload_js, upload_js_len,
                                 "application/javascript");
    return 0;
}

static int
handle_get_root(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)data;

    size_t buf_size = 4096;
    char  *buf = axl_malloc(buf_size);
    size_t written = 0;
    bool   as_json = wants_json(req);

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

static int
handle_get_path(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    ServeOpts *opts = (ServeOpts *)data;
    FtVolume   volume;
    char       sub_path[512];
    int        status;
    bool       is_dir;

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

    /* File download -- the SDK's response dispatcher (and our code
       below) currently materialize the full body in RAM before
       sending. Cap downloads so a misdirected GET on a huge ISO
       turns into a clean 413 instead of an OOM. Once the SDK has
       a streaming response API (see sdk-prompts/2026-05-10-axl-
       http-response-streamer.md), this cap can lift and we'll
       stream directly to the socket. */
#define WEBFS_MAX_GET_BODY  (256ULL * 1024 * 1024)  /* 256 MB */

    uint64_t file_size = 0;
    status = ft_get_file_size(&volume, sub_path, &file_size);
    if (status != 0) {
        axl_http_response_set_text(resp, "File not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    if (file_size > WEBFS_MAX_GET_BODY) {
        char msg[160];
        axl_snprintf(msg, sizeof(msg),
            "File is %llu bytes; serve currently caps GET at %llu "
            "bytes (streaming-response API pending).\n",
            (unsigned long long)file_size,
            (unsigned long long)WEBFS_MAX_GET_BODY);
        axl_http_response_set_text(resp, msg);
        axl_http_response_set_status(resp, 413);
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

/* Streaming PUT upload state.
 *
 * The SDK calls our handler N times for one PUT: once per chunk
 * (chunk != NULL, chunk_size > 0), then a final call with chunk=NULL,
 * chunk_size=0 where we set the response. UEFI is single-threaded
 * and the dispatcher is single-armed, so only one upload runs at a
 * time and these statics don't race.
 *
 * Failure handling: we keep consuming bytes after a per-chunk error
 * (just don't write them) so the dispatcher gets to the final call
 * with a clean state machine and we can emit the right status code
 * (404 / 500 / 201) explicitly. Returning non-zero mid-stream would
 * force a generic 500 from the SDK and lose our error context.
 *
 * Abort handling: if the TCP peer disconnects mid-upload, the SDK's
 * reset_connection path tears the conn down WITHOUT invoking the
 * upload handler with a final/abort signal -- m_put_open stays true
 * and m_put_ctx leaks into the next request. See
 * sdk-prompts/2026-05-10-upload-route-lifecycle.md for the proposed
 * SDK fix; meanwhile we recognize a hanging state at the start of
 * each call by comparing the current request path against the one
 * we saw on the last call. Different path with state still open
 * means abort recovery: discard the dangling FtWriteCtx. (Edge
 * case: same-path retry after abort still corrupts -- documented
 * in the prompt; gone once SDK abort signal lands.)
 */
static FtWriteCtx m_put_ctx;
static bool       m_put_open    = false;
static bool       m_put_failed  = false;
static int        m_put_status  = 201;
static char       m_put_path[512] = {0};

static int
handle_put_chunk(AxlHttpRequest *req, AxlHttpResponse *resp,
                 const void *chunk, size_t chunk_size, void *data)
{
    ServeOpts *opts = (ServeOpts *)data;
    bool is_final = (chunk == NULL && chunk_size == 0);

    /* Abort-recovery heuristic: a different request path arriving
       while state is still open means a prior upload was aborted.
       Discard the leaked write context. */
    if (m_put_open && req->path != NULL &&
        axl_strcmp(req->path, m_put_path) != 0) {
        ft_close_write(&m_put_ctx);
        m_put_open   = false;
        m_put_failed = false;
        m_put_status = 201;
    }

    /* First chunk OR final-with-empty-body: enforce read-only +
       parse path + open file. The SDK's middleware chain runs for
       regular routes only -- upload routes go straight to the
       handler -- so the read-only check that lives in
       permission_middleware has to be replicated here. See the
       same sdk-prompts entry for the middleware-bypass gap. */
    if (!m_put_open && !m_put_failed) {
        if (opts->read_only) {
            m_put_failed = true;
            m_put_status = 403;
        }
    }
    if (!m_put_open && !m_put_failed) {
        FtVolume volume;
        char     sub_path[512];

        if (parse_volume_path(req->path, &volume, sub_path,
                              sizeof(sub_path)) != 0) {
            m_put_failed = true;
            m_put_status = 404;
        } else if (ft_open_write(&volume, sub_path, NULL, NULL,
                                 &m_put_ctx) != 0) {
            m_put_failed = true;
            m_put_status = 500;
        } else {
            m_put_open = true;
            axl_strlcpy(m_put_path, req->path, sizeof(m_put_path));
        }
    }

    /* Stream chunk to disk. Failure flips us into "consume but don't
       write" until the final call. */
    if (!is_final && m_put_open && !m_put_failed) {
        if (ft_write_chunk(&m_put_ctx, chunk, chunk_size) != 0) {
            m_put_failed = true;
            m_put_status = 500;
        }
    }

    if (!is_final) {
        return 0;
    }

    /* Final call -- close, respond, reset. */
    if (m_put_open) {
        ft_close_write(&m_put_ctx);
        m_put_open = false;
    }

    const char *body;
    if (!m_put_failed) {
        body = "Created\n";
    } else if (m_put_status == 403) {
        body = "Read-only mode\n";
    } else if (m_put_status == 404) {
        body = "Volume not found\n";
    } else {
        body = "Write failed\n";
    }
    axl_http_response_set_text(resp, body);
    axl_http_response_set_status(resp, (size_t)m_put_status);
    publish_request(opts->loop, req, resp);

    /* Reset for the next upload. */
    m_put_failed  = false;
    m_put_status  = 201;
    m_put_path[0] = '\0';
    return 0;
}

static int
handle_delete_path(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    ServeOpts *opts = (ServeOpts *)data;
    FtVolume   volume;
    char       sub_path[512];
    int        status;

    status = parse_volume_path(req->path, &volume, sub_path, 512);
    if (status != 0) {
        axl_http_response_set_text(resp, "Volume not found\n");
        axl_http_response_set_status(resp, 404);
        publish_request(opts->loop, req, resp);
        return 0;
    }

    status = ft_delete(&volume, sub_path);
    if (status != 0) {
        axl_http_response_set_text(resp, "Not found\n");
        axl_http_response_set_status(resp, 404);
        publish_request(opts->loop, req, resp);
        return 0;
    }

    axl_http_response_set_text(resp, "Deleted\n");
    publish_request(opts->loop, req, resp);
    return 0;
}

static int
handle_post_path(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    ServeOpts *opts = (ServeOpts *)data;
    FtVolume   volume;
    char       sub_path[512];
    int        status;

    if (req->query == NULL || !axl_streql(req->query, "mkdir")) {
        axl_http_response_set_text(resp, "Use ?mkdir\n");
        axl_http_response_set_status(resp, 400);
        publish_request(opts->loop, req, resp);
        return 0;
    }

    status = parse_volume_path(req->path, &volume, sub_path, 512);
    if (status != 0) {
        axl_http_response_set_text(resp, "Volume not found\n");
        axl_http_response_set_status(resp, 404);
        publish_request(opts->loop, req, resp);
        return 0;
    }

    status = ft_mkdir(&volume, sub_path);
    if (status != 0) {
        axl_http_response_set_text(resp, "mkdir failed\n");
        axl_http_response_set_status(resp, 500);
        publish_request(opts->loop, req, resp);
        return 0;
    }

    axl_http_response_set_text(resp, "Created\n");
    axl_http_response_set_status(resp, 201);
    publish_request(opts->loop, req, resp);
    return 0;
}

// ----------------------------------------------------------------------------
// AxlService callbacks
// ----------------------------------------------------------------------------

static int
serve_setup(AxlLoop *loop, void *user)
{
    ServeOpts *o = (ServeOpts *)user;

    /* Attach the log file FIRST so subsequent setup output (banner,
       volume list, axl_warning/axl_error from later steps) reaches
       it. axl_log_file_attach handles open + handler registration
       + buffering internally; serve_teardown calls
       axl_log_file_detach for symmetric cleanup. On failure,
       surface a clear console error and continue -- a missing log
       destination shouldn't bring down the server. */
    if (o->log_path != NULL && o->log_path[0] != '\0') {
        if (axl_log_file_attach(o->log_path) != AXL_OK) {
            /* axl_printf (not axl_error) because the log facility
               we just failed to attach is what axl_error would
               route through -- console is the only sane channel
               for this specific failure. */
            axl_printf("ERROR: serve: cannot open log file '%s' "
                       "(read-only volume? bad path?) -- continuing "
                       "with console output only\n", o->log_path);
        }
    }

    /* Derive permission bools from the parsed mode string. */
    o->read_only  = axl_streql(o->mode, "read-only");
    o->write_only = axl_streql(o->mode, "write-only");

    size_t nic = (o->nic_index == (uint64_t)-1)
                 ? (size_t)-1
                 : (size_t)o->nic_index;
    if (network_init(nic, NULL, 10) != 0) {
        return AXL_ERR;
    }
    network_get_address(o->addr);

    if (ft_init() != 0) {
        network_cleanup();
        return AXL_ERR;
    }

    o->server = axl_http_server_new((uint16_t)o->port);
    if (o->server == NULL) {
        network_cleanup();
        return AXL_ERR;
    }

    if (o->source != NULL && o->source[0] != '\0') {
        axl_http_server_set(o->server, "listen.ip", o->source);
    }

    axl_http_server_set_body_limit(o->server, 128 * 1024 * 1024);  /* 128 MB */
    /* Single-threaded server -- keep-alive would block subsequent
       connections. */
    axl_http_server_set_keep_alive(o->server, 0);

    if (o->read_only || o->write_only) {
        axl_http_server_use(o->server, permission_middleware, o);
    }

    /* Routes. The SDK dispatcher tries exact matches before prefix
       patterns, so /_axl-webfs/upload.js shadows the GET wildcard
       regardless of registration order. */
    axl_http_server_add_route(o->server, "GET",    "/_axl-webfs/upload.js",
                              handle_get_upload_js, NULL);
    axl_http_server_add_route(o->server, "GET",    "/",  handle_get_root,    NULL);
    axl_http_server_add_route(o->server, "GET",    "/*", handle_get_path,    o);
    /* PUT goes through the streaming upload route so multi-GB
       uploads bypass body_limit and never materialize the whole
       request body in RAM. */
    axl_http_server_add_upload_route(o->server, "PUT", "/*",
                                     handle_put_chunk, o);
    axl_http_server_add_route(o->server, "DELETE", "/*", handle_delete_path, o);
    axl_http_server_add_route(o->server, "POST",   "/*", handle_post_path,   o);

    if (axl_http_server_start(o->server, loop) != AXL_OK) {
        axl_http_server_free(o->server);
        o->server = NULL;
        network_cleanup();
        return AXL_ERR;
    }

    /* Stash the loop on the opts struct so request handlers can
       publish on WEBFS_REQUEST_TOPIC; subscribe the default console
       printer for non-GET request feedback. Done after start
       succeeds so we don't leak a subscriber if start fails (the
       service framework doesn't call teardown on a failed setup). */
    o->loop = loop;
    o->request_sub_id = axl_pubsub_subscribe(loop, WEBFS_REQUEST_TOPIC,
                                             on_webfs_request, NULL);

    /* Recognizable single-line banner; goes through axl_info so the
       file log handler captures it when --log is set. */
    axl_info("listening on %d.%d.%d.%d:%llu (mode %s)",
             o->addr[0], o->addr[1], o->addr[2], o->addr[3],
             (unsigned long long)o->port, o->mode);

    /* Volume list -- ft_init has run by now. */
    size_t vcount = ft_get_volume_count();
    for (size_t i = 0; i < vcount; i++) {
        FtVolume vol;
        ft_get_volume(i, &vol);
        axl_info("  volume: %s", vol.name);
    }

    return AXL_OK;
}

static int
serve_teardown(void *user)
{
    ServeOpts *o = (ServeOpts *)user;

    if (o->loop != NULL && o->request_sub_id != 0) {
        (void)axl_pubsub_unsubscribe(o->loop, o->request_sub_id);
        o->request_sub_id = 0;
    }
    o->loop = NULL;

    if (o->server != NULL) {
        axl_http_server_free(o->server);
        o->server = NULL;
    }
    network_cleanup();

    /* Flush + close the log file before image unload (NULL-safe
       no-op when --log wasn't used). Symmetric with the
       axl_log_file_attach in setup. */
    axl_log_file_detach();
    return AXL_OK;
}

#endif /* AXL_SERVICE_BUILD_DRIVER */

const AxlService webfs_serve = {
    .name           = "axl-webfs-serve",
    .opts_descs     = serve_descs,
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = serve_setup,
    .teardown       = serve_teardown,
#endif
    .user           = &g_serve_opts,
    .driver_tick_ms = 50,
};

#ifdef AXL_SERVICE_BUILD_DRIVER
AXL_SERVICE_DRIVER(webfs_serve);
#endif
