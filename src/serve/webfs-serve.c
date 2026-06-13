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
#  include <axl/axl-cache.h>
#  include <axl/axl-digest.h>
#  include "serve/upload-asset.h"
#  include "serve/webfs-dav.h"
#  include "transfer/file-transfer.h"

#  define DIGEST_CACHE_SLOTS   16
#  define DIGEST_CACHE_TTL_MS  (5 * 60 * 1000)   /* 5 minutes */

typedef struct {
    uint64_t file_size;
    char     hex[65];   ///< 64 hex chars + NUL
} DigestSlot;

static AxlCache *m_digest_cache;

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
    /* AXL_NET_OPT_SERVER preset spelled out: --nic + --listen-ip +
       --port. As with mount, we spell the entries inline (not via
       runtime axl_config_descs_net) because the AxlService
       initializer requires a compile-time-const serve_descs[]. The
       canonical defaults come from <axl/axl-net-opts.h>:
       AXL_NET_NIC_AUTO_STR for the auto-NIC sentinel; default port
       9876 (the cross-tool default we settled on). NOTE: serve uses
       net.local_ip as the LISTEN-bind address; mount uses the same
       field as the outbound source IP — same bind(2) syscall, role
       implied by what the consumer does next. */
    { "nic",       AXL_CFG_UINT,   AXL_NET_NIC_AUTO_STR,
                                              "NIC index (auto if unset)",
      offsetof(ServeOpts, net.nic_index), sizeof(uint64_t) },
    { "listen-ip", AXL_CFG_STRING, "",           "Bind listen socket to this IPv4 (auto if empty)",
      offsetof(ServeOpts, net.local_ip),  sizeof(const char *) },
    { "port",      AXL_CFG_UINT,   "9876",       "Listen port (aligned with DEFAULT_SERVER_PORT / xfer-server.py)",
      offsetof(ServeOpts, net.port),      sizeof(uint16_t) },
    { "timeout",   AXL_CFG_UINT,   "0",          "Idle timeout in seconds (0 = none)",
      offsetof(ServeOpts, idle_timeout_sec), sizeof(uint64_t) },
    { "verbose",   AXL_CFG_BOOL,   "false",      "Verbose logging",
      offsetof(ServeOpts, verbose),          sizeof(bool) },
    { "mode",      AXL_CFG_STRING, "read-write", "Permission mode",
      offsetof(ServeOpts, mode),             sizeof(const char *) },
    { "log",       AXL_CFG_STRING, "",           "Log file path (empty = console only)",
      offsetof(ServeOpts, log_path),         sizeof(const char *) },
    { "auth",      AXL_CFG_STRING, "",           "HTTP Basic credential user:pass (empty = no auth)",
      offsetof(ServeOpts, auth),             sizeof(const char *) },
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

/* Compute (or recall) the SHA-256 of @p sub_path on @p volume.
   Result is cached by full path with the file size embedded — a
   size change invalidates the cached entry. Returns 0 on success
   with 64 lowercase hex chars + NUL written to @p out_hex; -1 on
   any failure (open, read, OOM). Callers that fail silently skip
   emitting the Digest header. Cache TTL is 5 minutes so write-then-
   read patterns refresh promptly.

   Non-static so webfs-dav.c (the AxlWebDavOps.digest adapter) can
   reuse the same cache across the REST and /dav surfaces. */
int
compute_file_digest(FtVolume *volume, const char *sub_path,
                    uint64_t file_size, char *out_hex)
{
    char cache_key[640];
    axl_snprintf(cache_key, sizeof(cache_key), "%s:%s",
                 volume->name, sub_path);

    DigestSlot slot;
    if (m_digest_cache != NULL &&
        axl_cache_get(m_digest_cache, cache_key, &slot) == AXL_OK &&
        slot.file_size == file_size) {
        axl_memcpy(out_hex, slot.hex, 65);
        return 0;
    }

    AxlChecksum *cs = axl_checksum_new(AXL_CHECKSUM_SHA256);
    if (cs == NULL) return -1;

    FtReadCtx rctx;
    if (ft_open_read(volume, sub_path, 0, NULL, NULL, &rctx) != 0) {
        axl_checksum_free(cs);
        return -1;
    }

    /* Hash the file in 64 KiB chunks. axl_fopen + axl_fread
       (via ft_read_chunk) keep memory bounded; sub-second for a
       few hundred MB on a typical UEFI FAT volume. */
    uint8_t buf[64 * 1024];
    while (true) {
        size_t got = 0;
        if (ft_read_chunk(&rctx, buf, sizeof(buf), &got) != 0) {
            ft_close_read(&rctx);
            axl_checksum_free(cs);
            return -1;
        }
        if (got == 0) break;
        axl_checksum_update(cs, buf, got);
    }
    ft_close_read(&rctx);

    const char *hex = axl_checksum_get_string(cs);
    if (hex == NULL) {
        axl_checksum_free(cs);
        return -1;
    }
    axl_memcpy(out_hex, hex, 65);
    axl_checksum_free(cs);

    if (m_digest_cache != NULL) {
        slot.file_size = file_size;
        axl_memcpy(slot.hex, out_hex, 65);
        axl_cache_put(m_digest_cache, cache_key, &slot);
    }
    return 0;
}

/* Streaming GET state (one per in-flight download). The SDK's
   axl_http_response_set_streamer pulls chunks via get_streamer_pull
   and finalizes via get_streamer_close on EOF, error, or connection
   reset -- closes the FtReadCtx and frees the context regardless of
   how the response ended. */
typedef struct {
    FtReadCtx ctx;
    uint64_t  remaining;
} GetStreamerCtx;

static int
get_streamer_pull(void *ctx, void *out_buf, size_t out_buf_size,
                  size_t *out_size)
{
    GetStreamerCtx *s = (GetStreamerCtx *)ctx;
    if (s->remaining == 0) {
        *out_size = 0;
        return AXL_OK;
    }

    size_t want = (s->remaining < (uint64_t)out_buf_size)
                  ? (size_t)s->remaining
                  : out_buf_size;
    size_t got = 0;
    if (ft_read_chunk(&s->ctx, out_buf, want, &got) != 0 || got == 0) {
        return AXL_ERR;
    }
    *out_size = got;
    s->remaining -= got;
    return AXL_OK;
}

static void
get_streamer_close(void *ctx)
{
    GetStreamerCtx *s = (GetStreamerCtx *)ctx;
    ft_close_read(&s->ctx);
    axl_free(s);
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

    /* File download -- streamed via axl_http_response_set_streamer
       (SDK 6218fa3). The dispatcher pulls chunks from
       get_streamer_pull and runs get_streamer_close on EOF / error
       / connection reset, so neither the file handle nor the
       streamer ctx ever leaks. No full-file malloc, no body-size
       cap. */
    uint64_t file_size = 0;
    status = ft_get_file_size(&volume, sub_path, &file_size);
    if (status != 0) {
        axl_http_response_set_text(resp, "File not found\n");
        axl_http_response_set_status(resp, 404);
        return 0;
    }

    /* Built-in SHA-256 integrity is OPT-IN per RFC 3230: the client
       sends `Want-Digest: sha-256` (or `sha256`) when it wants the
       server to emit a Digest header. Computing it on every GET
       would force a full-file read+hash that most clients never
       look at — measured ~3 minutes of overhead across the test
       suite when always-on. Mount clients ask explicitly; curl /
       browser GETs don't and pay nothing extra. */
    bool want_digest = false;
    const char *wd_hdr = (const char *)
        axl_hash_table_lookup(req->headers, "want-digest");
    if (wd_hdr != NULL) {
        if (axl_strcasestr(wd_hdr, "sha-256") != NULL ||
            axl_strcasestr(wd_hdr, "sha256") != NULL) {
            want_digest = true;
        }
    }

    char digest_hex[65];
    bool have_digest = false;
    if (want_digest) {
        have_digest =
            compute_file_digest(&volume, sub_path, file_size, digest_hex)
                == 0;
    }

    /* HEAD: emit metadata only, no body. The Digest insertion below
       still runs through the post-Range path so the table exists. */
    bool is_head = axl_streql(req->method, "HEAD");
    if (is_head) {
        axl_http_response_set_status(resp, 200);
        /* Set Content-Length so HEAD clients can size their reads,
           per RFC 9110 §15.4.5. Don't install a streamer — the SDK's
           HEAD path skips bodies. */
    }

    /* Range support: open at the requested start offset and bound
       the streamer to the slice length. The 206-status + the
       Content-Range header come from us; the SDK's
       axl_http_response_set_content_range formats and inserts the
       header for the streaming case (the buffered set_range
       auto-emits it itself). */
    uint64_t start_offset = 0;
    uint64_t slice_len    = file_size;
    uint64_t range_end    = 0;  /* inclusive, only valid when is_range */
    bool     is_range     = false;

    const char *range_hdr =
        (const char *)axl_hash_table_lookup(req->headers, "range");
    if (range_hdr != NULL) {
        AxlHttpRange range;
        if (axl_http_parse_range(range_hdr, file_size, &range)) {
            start_offset = range.start;
            range_end    = range.end;
            slice_len    = range.end - range.start + 1;
            is_range     = true;
        }
    }

    GetStreamerCtx *sctx = axl_calloc(1, sizeof(*sctx));
    if (sctx == NULL) {
        axl_http_response_set_text(resp, "Out of memory\n");
        axl_http_response_set_status(resp, 500);
        return 0;
    }

    if (ft_open_read(&volume, sub_path, start_offset, NULL, NULL,
                     &sctx->ctx) != 0) {
        axl_free(sctx);
        axl_http_response_set_text(resp, "Cannot open file\n");
        axl_http_response_set_status(resp, 500);
        return 0;
    }
    sctx->remaining = slice_len;

    if (is_head) {
        /* HEAD: tear down the streamer ctx and emit just headers.
           Closing the FtReadCtx now is safe — the streamer would
           have done it on EOF or reset_connection. */
        ft_close_read(&sctx->ctx);
        axl_free(sctx);
    } else {
        axl_http_response_set_streamer(resp, get_streamer_pull, sctx,
                                       get_streamer_close,
                                       (size_t)slice_len,
                                       "application/octet-stream");
        if (is_range) {
            axl_http_response_set_content_range(resp, start_offset,
                                                range_end, file_size);
            axl_http_response_set_status(resp, 206);
        } else {
            axl_http_response_set_status(resp, 200);
        }
    }

    /* Digest: must be inserted AFTER set_content_range so the SDK
       lazy-allocates resp->headers with its own free-func contract
       (str-table-with-free; see axl_http_parse_headers). Inserting
       into a NULL or differently-shaped table earlier silently
       loses the entry. */
    if (have_digest) {
        if (resp->headers == NULL) {
            /* No Range path ran (full-file GET or HEAD) — match the
               SDK's contract by allocating the same shape. */
            resp->headers = axl_hash_table_new_str();
        }
        if (resp->headers != NULL) {
            char digest_val[80];
            axl_snprintf(digest_val, sizeof(digest_val),
                         "sha-256=%s", digest_hex);
            axl_hash_table_insert(resp->headers, "Digest",
                                  axl_strdup(digest_val));
        }
    }
    return 0;
}

/* Streaming PUT upload state.
 *
 * The SDK calls our handler N times for one PUT: once per chunk
 * (chunk != NULL, chunk_size > 0), then EITHER a clean-EOF final
 * call (chunk=NULL, chunk_size=0, aborted=false) where we set the
 * response, OR an abort call (same NULL/0 with aborted=true) when
 * the TCP peer disconnected mid-upload. The two terminal calls are
 * mutually exclusive (SDK 2341dcc). UEFI is single-threaded and the
 * dispatcher is single-armed, so only one upload runs at a time
 * and these statics don't race.
 *
 * Read-only enforcement runs in permission_middleware, which the
 * SDK now invokes ahead of upload routes (1eb3fc0).
 *
 * Failure handling: we keep consuming bytes after a per-chunk error
 * (just don't write them) so the dispatcher reaches the final call
 * with a clean state machine and we can emit the right status code
 * (404 / 500 / 201) explicitly. Returning non-zero mid-stream would
 * force a generic 500 from the SDK and lose our error context.
 */
static FtWriteCtx m_put_ctx;
static bool       m_put_open    = false;
static bool       m_put_failed  = false;
static int        m_put_status  = 201;

static int
handle_put_chunk(AxlHttpRequest *req, AxlHttpResponse *resp,
                 const void *chunk, size_t chunk_size, void *data,
                 bool aborted)
{
    ServeOpts *opts = (ServeOpts *)data;

    /* Abort: TCP teardown mid-upload. resp is NOT transmitted on
       this call -- our only job is to release per-request state. */
    if (aborted) {
        if (m_put_open) {
            ft_close_write(&m_put_ctx);
            m_put_open = false;
        }
        m_put_failed = false;
        m_put_status = 201;
        return 0;
    }

    bool is_final = (chunk == NULL && chunk_size == 0);

    /* First chunk OR final-with-empty-body: parse path + open file. */
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
    } else if (m_put_status == 404) {
        body = "Volume not found\n";
    } else {
        body = "Write failed\n";
    }
    axl_http_response_set_text(resp, body);
    axl_http_response_set_status(resp, (size_t)m_put_status);
    publish_request(opts->loop, req, resp);

    /* Reset for the next upload. */
    m_put_failed = false;
    m_put_status = 201;
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
// Authentication (HTTP Basic)
// ----------------------------------------------------------------------------

/* Validate "Authorization: Basic <base64(user:pass)>" against the
   configured --auth credential. The configured string is already the
   exact "user:pass" form a Basic header decodes to, so we compare the
   decoded bytes directly — no split needed. Only registered when
   o->auth_enabled; gated routes return 401 otherwise.

   NOTE (SDK gap): the SDK's 401 path emits no WWW-Authenticate header
   and the callback can't reach the response, so interactive browsers /
   Finder are never prompted to send credentials. Gating works for
   clients that send Basic preemptively (curl -u, a configured davfs2).
   Surfaced to axl-sdk; revisit once it emits a challenge. */
static int
webfs_auth_cb(AxlHttpRequest *req, AxlAuthInfo *auth_out, void *data)
{
    ServeOpts *o = (ServeOpts *)data;

    const char *hdr = axl_hash_table_lookup(req->headers, "authorization");
    if (hdr == NULL || axl_strncasecmp(hdr, "Basic ", 6) != 0)
        return AXL_ERR;

    void  *decoded = NULL;
    size_t dlen = 0;
    if (axl_base64_decode(hdr + 6, &decoded, &dlen) != AXL_OK || decoded == NULL)
        return AXL_ERR;

    /* decoded is not NUL-terminated — compare by exact length. */
    bool ok = (dlen == axl_strlen(o->auth)) &&
              (axl_memcmp(decoded, o->auth, dlen) == 0);
    axl_free(decoded);
    if (!ok)
        return AXL_ERR;

    auth_out->username = o->auth_user;
    auth_out->role     = AXL_ROUTE_ADMIN;
    return AXL_OK;
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

    /* Derive HTTP Basic auth state. When --auth is "user:pass", every
       route (REST + uploads + /dav) is gated through webfs_auth_cb.
       Cache the username (substring before ':') for AxlAuthInfo. */
    o->auth_enabled = o->auth != NULL && o->auth[0] != '\0';
    if (o->auth_enabled) {
        const char *colon = axl_strchr(o->auth, ':');
        size_t ulen = colon != NULL ? (size_t)(colon - o->auth)
                                    : axl_strlen(o->auth);
        if (ulen >= sizeof(o->auth_user))
            ulen = sizeof(o->auth_user) - 1;
        axl_memcpy(o->auth_user, o->auth, ulen);
        o->auth_user[ulen] = '\0';
    }

    if (axl_net_init_from_opts(&o->net, 10) != AXL_OK) {
        return AXL_ERR;
    }
    /* Cache the bound IPv4 for the banner line below.
       axl_net_get_ip_address returns the first configured NIC's
       address — exactly what we want post-DHCP. AxlIPv4Address.addr
       is uint8_t[4] so the memcpy is a same-layout copy. */
    {
        AxlIPv4Address tmp;
        if (axl_net_get_ip_address(&tmp) == AXL_OK) {
            axl_memcpy(o->addr, tmp.addr, sizeof(o->addr));
        }
    }

    if (ft_init() != 0) {
        return AXL_ERR;
    }

    /* SHA-256 digest cache for GET responses. Capped at 16 slots
       with a 5-minute TTL — recomputes promptly after writes
       without spending CPU on a freshly-hashed file every request. */
    m_digest_cache = axl_cache_new(DIGEST_CACHE_SLOTS,
                                   sizeof(DigestSlot),
                                   DIGEST_CACHE_TTL_MS);

    /* net.port is already uint16_t — no truncation cast needed. */
    o->server = axl_http_server_new(o->net.port);
    if (o->server == NULL) {
        return AXL_ERR;
    }

    if (o->net.local_ip != NULL && o->net.local_ip[0] != '\0') {
        axl_http_server_set(o->server, "listen.ip", o->net.local_ip);
    }

    axl_http_server_set_body_limit(o->server, 128 * 1024 * 1024);  /* 128 MB */
    /* Single-threaded server -- keep-alive would block subsequent
       connections. */
    axl_http_server_set_keep_alive(o->server, 0);

    if (o->read_only || o->write_only) {
        axl_http_server_use(o->server, permission_middleware, o);
    }

    /* HTTP Basic gate. When --auth is set, register the credential
       checker and require an authenticated user on every route
       (AXL_ROUTE_AUTH); otherwise AXL_ROUTE_NO_AUTH leaves the routes
       open — identical to the non-_auth registration variants. */
    uint32_t auth_flags = AXL_ROUTE_NO_AUTH;
    if (o->auth_enabled) {
        axl_http_server_use_auth(o->server, webfs_auth_cb, o);
        auth_flags = AXL_ROUTE_AUTH;
    }

    /* Routes. The SDK dispatcher tries exact matches before prefix
       patterns, so /_axl-webfs/upload.js shadows the GET wildcard
       regardless of registration order. */
    axl_http_server_add_route_auth(o->server, "GET",    "/_axl-webfs/upload.js",
                              handle_get_upload_js, NULL, auth_flags);
    axl_http_server_add_route_auth(o->server, "GET",    "/",  handle_get_root,    NULL, auth_flags);
    axl_http_server_add_route_auth(o->server, "GET",    "/*", handle_get_path,    o, auth_flags);
    /* HEAD routes to the same handler — emits Digest + status without
       a body. Mount clients use this to fetch SHA-256 on Open. */
    axl_http_server_add_route_auth(o->server, "HEAD",   "/*", handle_get_path,    o, auth_flags);
    /* PUT goes through the streaming upload route so multi-GB
       uploads bypass body_limit and never materialize the whole
       request body in RAM. The _auth variant enforces auth_flags
       before any body byte is accepted (streaming uploads bypass the
       dispatch-time check). */
    axl_http_server_add_upload_route_auth(o->server, "PUT", "/*",
                                     handle_put_chunk, o, auth_flags);
    axl_http_server_add_route_auth(o->server, "DELETE", "/*", handle_delete_path, o, auth_flags);
    axl_http_server_add_route_auth(o->server, "POST",   "/*", handle_post_path,   o, auth_flags);

    /* WebDAV class-1 + MOVE under /dav. Modern clients (Finder,
       Explorer, davfs2, cadaver) mount this directly. The SDK owns
       all protocol bits; webfs-dav.c just wires AxlWebDavOps onto
       the ft_* helpers. Registered after the catch-all wildcard
       routes so the SDK's prefix matcher routes /dav/... to the
       WebDAV dispatcher and leaves /<vol>/... on the REST surface.
       Read-only / write-only enforcement composes through
       permission_middleware (SDK 1eb3fc0); auth_flags gates every
       verb route of the mount uniformly. */
    webfs_dav_register(o->server, "/dav", auth_flags);

    if (axl_http_server_start(o->server, loop) != AXL_OK) {
        axl_http_server_free(o->server);
        o->server = NULL;
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
    axl_info("listening on %d.%d.%d.%d:%u (mode %s)",
             o->addr[0], o->addr[1], o->addr[2], o->addr[3],
             (unsigned)o->net.port, o->mode);

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
    if (m_digest_cache != NULL) {
        axl_cache_free(m_digest_cache);
        m_digest_cache = NULL;
    }

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
