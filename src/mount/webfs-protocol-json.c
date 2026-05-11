/** @file
  axl-webfs-mount -- bespoke HTTP/JSON protocol implementation.

  Original mount protocol, paired with xfer-server.py's default
  (non-WebDAV) mode. See webfs-protocol.h for the verb mapping.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-protocol.h"
#include <axl/axl-json.h>

// ----------------------------------------------------------------------------
// probe — GET /info
// ----------------------------------------------------------------------------

static int
proto_probe(WebFsPrivate *priv)
{
    char info_url[320];
    axl_snprintf(info_url, sizeof(info_url), "%s/info", priv->base_url);
    AxlHttpClientResponse *resp = NULL;
    int rc = axl_http_get(priv->http_client, info_url, &resp);
    int ok = (rc == 0 && resp != NULL && resp->status_code == 200);
    if (resp != NULL) axl_http_client_response_free(resp);
    return ok ? 0 : -1;
}

// ----------------------------------------------------------------------------
// list_dir — GET /list/<path>, JSON array
// ----------------------------------------------------------------------------

static int
proto_list_dir(WebFsPrivate *priv, const char *path,
               DirEntry *out, size_t max, size_t *out_count)
{
    *out_count = 0;

    char list_path[MAX_PATH_LEN];
    axl_snprintf(list_path, sizeof(list_path), "/list%s", path);

    AxlHttpClientResponse *response = NULL;
    int rc = webfs_http_request(priv, "GET", list_path, NULL, NULL, 0,
                                &response);
    if (rc != 0 || response == NULL) return -1;
    if (response->status_code != 200) {
        axl_http_client_response_free(response);
        return -1;
    }

    size_t body_size = response->body_size;
    if (body_size >= HTTP_BODY_BUF_SIZE) body_size = HTTP_BODY_BUF_SIZE - 1;
    char *body_buf = axl_malloc(body_size + 1);
    if (body_buf == NULL) {
        axl_http_client_response_free(response);
        return -1;
    }
    axl_memcpy(body_buf, response->body, body_size);
    body_buf[body_size] = '\0';
    axl_http_client_response_free(response);

    AxlJsonReader ctx;
    if (!axl_json_parse(body_buf, body_size, &ctx)) {
        axl_free(body_buf);
        return -1;
    }

    AxlJsonArrayIter iter;
    if (!axl_json_root_array_begin(&ctx, &iter)) {
        axl_json_free(&ctx);
        axl_free(body_buf);
        return -1;
    }

    size_t n = 0;
    AxlJsonReader elem;
    while (axl_json_array_next(&iter, &elem) && n < max) {
        DirEntry *e = &out[n];
        axl_memset(e, 0, sizeof(*e));
        axl_json_get_string(&elem, "name", e->name, sizeof(e->name));
        axl_json_get_uint(&elem, "size", &e->size);
        axl_json_get_bool(&elem, "dir", &e->is_dir);
        axl_json_get_string(&elem, "modified", e->modified,
                            sizeof(e->modified));
        if (e->name[0] != '\0') n++;
    }

    axl_json_free(&ctx);
    axl_free(body_buf);

    *out_count = n;
    return 0;
}

// ----------------------------------------------------------------------------
// read_range — GET /files/<path> + Range
// ----------------------------------------------------------------------------

static int
proto_read_range(WebFsPrivate *priv, const char *path,
                 uint64_t offset, size_t len,
                 void *buf, size_t *out_got,
                 char *out_digest_hex, size_t digest_size)
{
    *out_got = 0;
    if (len == 0) return 0;

    char range_val[64];
    axl_snprintf(range_val, sizeof(range_val), "bytes=%llu-%llu",
                 (unsigned long long)offset,
                 (unsigned long long)(offset + len - 1));
    AxlHashTable *hdrs = axl_hash_table_new_str();
    if (hdrs == NULL) return -1;
    axl_hash_table_insert(hdrs, "range", axl_strdup(range_val));
    /* RFC 3230 Want-Digest: only ask the server to hash the file
       when the caller actually plans to validate. Saves a full-file
       read+SHA-256 on the server side per non-validated GET. */
    if (out_digest_hex != NULL) {
        axl_hash_table_insert(hdrs, "want-digest", axl_strdup("sha-256"));
    }

    char file_path[MAX_PATH_LEN];
    axl_snprintf(file_path, sizeof(file_path), "/files%s", path);

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "GET", file_path, hdrs, NULL, 0,
                                &resp);
    axl_free(axl_hash_table_lookup(hdrs, "range"));
    if (out_digest_hex != NULL) {
        axl_free(axl_hash_table_lookup(hdrs, "want-digest"));
    }
    axl_hash_table_free(hdrs);
    if (rc != 0 || resp == NULL) return -1;
    if (resp->status_code != 200 && resp->status_code != 206) {
        axl_http_client_response_free(resp);
        return -1;
    }

    size_t got = resp->body_size;
    if (got > len) got = len;
    axl_memcpy(buf, resp->body, got);
    if (out_digest_hex != NULL && resp->headers != NULL) {
        const char *d = axl_hash_table_lookup(resp->headers, "digest");
        if (d != NULL) {
            webfs_digest_parse_sha256(d, out_digest_hex, digest_size);
        }
    }
    axl_http_client_response_free(resp);
    *out_got = got;
    return 0;
}

// ----------------------------------------------------------------------------
// write_full / create_empty — PUT /files/<path>
// ----------------------------------------------------------------------------

static int
proto_write_full(WebFsPrivate *priv, const char *path,
                 const void *body, size_t len, size_t *out_status)
{
    char file_path[MAX_PATH_LEN];
    axl_snprintf(file_path, sizeof(file_path), "/files%s", path);

    /* Stream the body through axl_http_request_streaming (SDK
       14cef93) so the client doesn't materialize a second
       body-sized buffer. For multi-hundred-MB cp from UEFI Shell
       this halves peak RSS. */
    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request_buf_streaming(
        priv, "PUT", file_path, NULL, body, len,
        "application/octet-stream", &resp);
    if (rc != 0 || resp == NULL) return -1;
    *out_status = resp->status_code;
    axl_http_client_response_free(resp);
    return 0;
}

static int
proto_create_empty(WebFsPrivate *priv, const char *path, size_t *out_status)
{
    /* JSON variant historically sent an empty-string body. Keep that
       shape (it's how xfer-server.py distinguishes "client knew it
       was empty" from "client forgot Content-Length"). */
    return proto_write_full(priv, path, "", 0, out_status);
}

// ----------------------------------------------------------------------------
// mkdir — POST /files/<path>?mkdir
// ----------------------------------------------------------------------------

static int
proto_mkdir(WebFsPrivate *priv, const char *path, size_t *out_status)
{
    char mkdir_path[MAX_PATH_LEN];
    axl_snprintf(mkdir_path, sizeof(mkdir_path), "/files%s?mkdir", path);

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "POST", mkdir_path, NULL, NULL, 0,
                                &resp);
    if (rc != 0 || resp == NULL) return -1;
    *out_status = resp->status_code;
    axl_http_client_response_free(resp);
    return 0;
}

// ----------------------------------------------------------------------------
// remove — DELETE /files/<path>
// ----------------------------------------------------------------------------

static int
proto_remove(WebFsPrivate *priv, const char *path, size_t *out_status)
{
    char file_path[MAX_PATH_LEN];
    axl_snprintf(file_path, sizeof(file_path), "/files%s", path);

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "DELETE", file_path, NULL, NULL, 0,
                                &resp);
    if (rc != 0) {
        if (resp != NULL) axl_http_client_response_free(resp);
        return -1;
    }
    *out_status = (resp != NULL) ? resp->status_code : 0;
    if (resp != NULL) axl_http_client_response_free(resp);
    return 0;
}

// ----------------------------------------------------------------------------
// rename — POST /files/<old>?rename=<new>
// ----------------------------------------------------------------------------

static int
proto_rename(WebFsPrivate *priv, const char *old_path,
             const char *new_name, size_t *out_status)
{
    /* URL-encode the new basename so spaces / + / & / # / etc.
       survive the query string. webfs_http_request URL-encodes the
       path portion but leaves the query verbatim, so anything past
       the ? is on us. */
    char new_name_enc[256 * 3];
    axl_url_encode(new_name, new_name_enc, sizeof(new_name_enc));
    char rename_path[MAX_PATH_LEN + sizeof(new_name_enc)];
    axl_snprintf(rename_path, sizeof(rename_path),
                 "/files%s?rename=%s", old_path, new_name_enc);

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "POST", rename_path, NULL, NULL, 0,
                                &resp);
    if (rc != 0) {
        if (resp != NULL) axl_http_client_response_free(resp);
        return -1;
    }
    *out_status = (resp != NULL) ? resp->status_code : 0;
    if (resp != NULL) axl_http_client_response_free(resp);
    return 0;
}

// ----------------------------------------------------------------------------
// Vtable
// ----------------------------------------------------------------------------

const WebfsProtocolOps webfs_proto_json = {
    .probe        = proto_probe,
    .list_dir     = proto_list_dir,
    .read_range   = proto_read_range,
    .write_full   = proto_write_full,
    .create_empty = proto_create_empty,
    .mkdir        = proto_mkdir,
    .remove       = proto_remove,
    .rename       = proto_rename,
};
