/** @file
  axl-webfs-mount -- RFC 4918 WebDAV class-1 + MOVE client.

  Implements the same WebfsProtocolOps callbacks the JSON path
  serves, talking standard WebDAV instead. Read/write go through
  plain GET/PUT (unchanged from the JSON path's wire shape);
  PROPFIND drives directory listings, MKCOL/MOVE/DELETE replace
  the bespoke ?mkdir / ?rename query strings.

  PROPFIND XML is parsed via axl-sdk's AxlXmlReader pull-token
  reader. We only consume the four properties mount actually
  cares about: resourcetype (collection vs file),
  getcontentlength (file size), getlastmodified (RFC 1123 date
  string -- displayed as-is), displayname (the entry's
  basename). Everything else (lockdiscovery, supportedlock,
  getetag, creationdate, ...) gets skipped.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-protocol.h"
#include <axl/axl-xml.h>

AXL_LOG_DOMAIN("webfs-dav-client");

// ----------------------------------------------------------------------------
// Path helpers
// ----------------------------------------------------------------------------

/// PROPFIND <D:href> bodies always carry the URL-encoded path. Decode
/// in place to UTF-8 bytes the mount layer expects, then strip the
/// scheme://host prefix if present so we keep just the path portion.
static bool
decode_href(const char *href, char *out, size_t out_size)
{
    /* @p href is the WHOLE encoded text, however long (it may be three
       times the decoded path). Only the decoded path is held to
       @p out_size, and one that does not fit is refused (false), never
       cut: a cut here cuts the basename. */
    const char *p = href;
    if (axl_strncmp(p, "http://", 7) == 0) p += 7;
    else if (axl_strncmp(p, "https://", 8) == 0) p += 8;
    if (p != href) {
        const char *slash = p;
        while (*slash && *slash != '/') slash++;
        p = (*slash == '/') ? slash : "/";
    }
    return axl_url_decode(p, out, out_size) >= 0;
}

/// Extract the basename from a path (last "/"-delimited component).
/// Trailing slash is stripped first so "/foo/bar/" yields "bar".
/// false when it does not fit @p out_size (WEBFS_NAME_SIZE: skip, never
/// cut).
static bool
href_basename(const char *path, char *out, size_t out_size)
{
    size_t len = axl_strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') start = i + 1;
    }
    size_t bn = len - start;
    if (bn >= out_size) {
        out[0] = '\0';
        return false;
    }
    axl_memcpy(out, path + start, bn);
    out[bn] = '\0';
    return true;
}

// ----------------------------------------------------------------------------
// probe — OPTIONS / and check for DAV: header
// ----------------------------------------------------------------------------

static int
proto_probe(WebFsPrivate *priv)
{
    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "OPTIONS", "/", NULL, NULL, 0, &resp);
    if (rc != 0 || resp == NULL) return -1;

    int ok = 0;
    if (resp->status_code >= 200 && resp->status_code < 400 &&
        resp->headers != NULL) {
        const char *dav = axl_hash_table_lookup(resp->headers, "dav");
        if (dav != NULL && dav[0] != '\0') ok = 1;
    }
    axl_http_client_response_free(resp);
    return ok ? 0 : -1;
}

// ----------------------------------------------------------------------------
// list_dir — PROPFIND Depth: 1
// ----------------------------------------------------------------------------

/* The PROPFIND body asks for "allprop" -- works against every
   WebDAV server we care about (wsgidav, Apache mod_dav, NextCloud,
   IIS). Strict servers that return more verbose listings just give
   us more fields to ignore; pull-token parsing is robust to that. */
static const char k_propfind_allprop[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:allprop/></D:propfind>";

/// PROPFIND parser state: one <D:response> at a time, accumulating
/// the four fields we need before the matching </D:response> closes.
typedef struct {
    char     href[MAX_PATH_LEN];
    char     name[WEBFS_NAME_SIZE];
    uint64_t size;
    bool     is_collection;
    bool     have_href;
    bool     too_long;          ///< href or displayname did not fit: skip
    bool     have_resourcetype;
    bool     have_size;
} DavEntry;

static void
dav_entry_reset(DavEntry *e)
{
    axl_memset(e, 0, sizeof(*e));
}

/// Emit the parser's accumulated entry into the caller's DirEntry
/// buffer, mapping WebDAV semantics to mount's DirEntry contract.
/// Returns true iff the entry should be appended.
static bool
dav_entry_emit(const DavEntry *e, const char *parent_path,
               DirEntry *out)
{
    if (!e->have_href) return false;
    if (e->too_long) {
        axl_warning("listing %s: skipping an entry whose name or href does "
                    "not fit (names up to %zu bytes)", parent_path,
                    sizeof(out->name) - 1);
        return false;
    }

    /* Skip the self entry — PROPFIND Depth:1 returns the queried
       resource plus its children. The self entry is whichever <href>
       resolves to the parent_path (modulo trailing slash). Both sides
       have to compare in the same encoding; decode_href already
       percent-decoded e->href, so percent-decode parent_path here
       too. Non-ASCII / +-containing paths would otherwise leak the
       directory itself into the listing as a child. */
    char parent_dec[MAX_PATH_LEN];
    axl_url_decode(parent_path, parent_dec, sizeof(parent_dec));
    size_t hlen = axl_strlen(e->href);
    size_t plen = axl_strlen(parent_dec);
    while (hlen > 1 && e->href[hlen - 1] == '/') hlen--;
    while (plen > 1 && parent_dec[plen - 1] == '/') plen--;
    if (hlen == plen && axl_memcmp(e->href, parent_dec, hlen) == 0)
        return false;

    axl_memset(out, 0, sizeof(*out));
    if (e->name[0] != '\0') {
        axl_strlcpy(out->name, e->name, sizeof(out->name));   /* fits */
    } else if (!href_basename(e->href, out->name, sizeof(out->name))) {
        axl_warning("listing %s: skipping an entry whose name is longer "
                    "than %zu bytes", parent_path, sizeof(out->name) - 1);
        return false;
    }
    out->is_dir = e->is_collection;
    out->size   = e->is_collection ? 0 : e->size;
    return out->name[0] != '\0';
}

static int
proto_list_dir(WebFsPrivate *priv, const char *path, AxlArray *out)
{
    AxlHashTable *hdrs = axl_hash_table_new_str();
    if (hdrs == NULL) return -1;
    axl_hash_table_insert(hdrs, "depth", axl_strdup("1"));
    axl_hash_table_insert(hdrs, "content-type",
                          axl_strdup("application/xml; charset=\"utf-8\""));

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "PROPFIND", path, hdrs,
                                k_propfind_allprop,
                                sizeof(k_propfind_allprop) - 1, &resp);
    axl_free(axl_hash_table_lookup(hdrs, "depth"));
    axl_free(axl_hash_table_lookup(hdrs, "content-type"));
    axl_hash_table_free(hdrs);

    if (rc != 0 || resp == NULL) return -1;
    if (resp->status_code != 207) {
        axl_http_client_response_free(resp);
        return -1;
    }

    AxlXmlReader *r = axl_xml_reader_new(resp->body, resp->body_size);
    if (r == NULL) {
        axl_http_client_response_free(resp);
        return -1;
    }

    /* Element-stack model:
         multistatus → response → href / propstat → status / prop → <field>

       Apache mod_dav and NextCloud emit multiple <D:propstat> blocks
       per resource — one with HTTP/1.1 200 OK for supported props,
       another with 404 / 403 for props the server doesn't expose.
       The 404 block typically carries empty property elements; if we
       captured them, they'd clobber the real values from the 200
       block.

       Order matters and rules out gate-before-status: wsgidav puts
       <D:prop> before <D:status> inside <D:propstat>, so the status
       isn't known until after all fields have parsed. Instead, we
       snapshot `cur` on <propstat> entry and restore it if the
       block's <D:status> doesn't carry "200". The 200 block always
       wins; non-200 blocks are no-ops by reversal. */
    DavEntry cur;
    DavEntry propstat_snapshot;
    dav_entry_reset(&cur);
    dav_entry_reset(&propstat_snapshot);

    bool   in_response       = false;
    bool   in_href           = false;
    bool   in_propstat       = false;
    bool   in_status         = false;
    bool   propstat_ok       = true;   /* default-accept; flipped on bad status */
    bool   in_displayname    = false;
    bool   in_resourcetype   = false;
    bool   in_size           = false;
    bool   oom = false;
    /* An element's text can arrive as several TEXT tokens -- a CDATA
       section splits it ("a<![CDATA[&b]]>c" is three) -- so href and
       displayname accumulate here and are taken whole at the element's
       END. The reader has already decoded the entities. */
    AxlString *text = axl_string_new(NULL);
    if (text == NULL) {
        axl_xml_reader_free(r);
        axl_http_client_response_free(resp);
        return -1;
    }

    AxlXmlToken tok;
    while (!oom && axl_xml_reader_next(r, &tok)) {
        if (tok.type == AXL_XML_TOKEN_START_ELEMENT) {
            if (axl_xml_token_local_name_eq(&tok, "response")) {
                dav_entry_reset(&cur);
                in_response = true;
            } else if (!in_response) {
                /* outside <response>, ignore */
            } else if (axl_xml_token_local_name_eq(&tok, "href")) {
                in_href = true;
                axl_string_clear(text);
            } else if (axl_xml_token_local_name_eq(&tok, "propstat")) {
                in_propstat = true;
                propstat_ok = true;
                propstat_snapshot = cur;
            } else if (in_propstat &&
                       axl_xml_token_local_name_eq(&tok, "status")) {
                in_status = true;
            } else if (axl_xml_token_local_name_eq(&tok, "displayname")) {
                in_displayname = true;
                axl_string_clear(text);
            } else if (axl_xml_token_local_name_eq(&tok, "resourcetype")) {
                in_resourcetype = true;
                cur.have_resourcetype = true;
            } else if (axl_xml_token_local_name_eq(&tok,
                                                   "getcontentlength")) {
                in_size = true;
            } else if (in_resourcetype &&
                       axl_xml_token_local_name_eq(&tok, "collection")) {
                cur.is_collection = true;
            }
        } else if (tok.type == AXL_XML_TOKEN_TEXT) {
            if (in_status) {
                /* "HTTP/1.1 200 OK" — accept iff "200" appears. */
                bool ok = false;
                for (size_t i = 0; i + 2 < tok.text_len; i++) {
                    if (tok.text[i] == '2' && tok.text[i + 1] == '0' &&
                        tok.text[i + 2] == '0') {
                        ok = true;
                        break;
                    }
                }
                propstat_ok = ok;
            } else if (in_href || in_displayname) {
                if (axl_string_append_len(text, tok.text, tok.text_len)
                        != AXL_OK)
                    cur.too_long = true;   /* OOM: skip, never cut */
            } else if (in_size) {
                /* getcontentlength text is ASCII digits */
                cur.size = 0;
                for (size_t i = 0; i < tok.text_len; i++) {
                    char c = tok.text[i];
                    if (c < '0' || c > '9') break;
                    cur.size = cur.size * 10 + (uint64_t)(c - '0');
                }
                cur.have_size = true;
            }
        } else if (tok.type == AXL_XML_TOKEN_END_ELEMENT) {
            if (in_href &&
                axl_xml_token_local_name_eq(&tok, "href")) {
                in_href = false;
                if (!decode_href(axl_string_str(text), cur.href,
                                 sizeof(cur.href)))
                    cur.too_long = true;
                cur.have_href = true;
            } else if (in_status &&
                       axl_xml_token_local_name_eq(&tok, "status")) {
                in_status = false;
            } else if (in_displayname &&
                       axl_xml_token_local_name_eq(&tok, "displayname")) {
                in_displayname = false;
                size_t len = axl_string_len(text);
                if (len >= sizeof(cur.name)) {
                    cur.too_long = true;
                    cur.name[0] = '\0';
                } else {
                    axl_memcpy(cur.name, axl_string_str(text), len);
                    cur.name[len] = '\0';
                }
            } else if (in_size &&
                       axl_xml_token_local_name_eq(&tok,
                                                   "getcontentlength")) {
                in_size = false;
            } else if (in_resourcetype &&
                       axl_xml_token_local_name_eq(&tok, "resourcetype")) {
                in_resourcetype = false;
            } else if (in_propstat &&
                       axl_xml_token_local_name_eq(&tok, "propstat")) {
                in_propstat = false;
                /* Revert this block's captures if the server tagged
                   it as anything but 200. The 200 block — which we
                   accept by default — keeps its captures. */
                if (!propstat_ok) cur = propstat_snapshot;
                propstat_ok = true;
            } else if (in_response &&
                       axl_xml_token_local_name_eq(&tok, "response")) {
                in_response = false;
                DirEntry entry;
                if (dav_entry_emit(&cur, path, &entry) &&
                    axl_array_append(out, &entry) != AXL_OK)
                    oom = true;   /* a short listing would pass for whole */
            }
        }
    }

    bool parse_err = axl_xml_reader_error(r, NULL, NULL, NULL);
    axl_string_free(text);
    axl_xml_reader_free(r);
    axl_http_client_response_free(resp);

    if (parse_err) {
        axl_warning("PROPFIND XML parse error on %s", path);
        return -1;
    }

    return oom ? -1 : 0;
}

// ----------------------------------------------------------------------------
// read_range — GET + Range (identical wire shape to JSON path)
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
    if (out_digest_hex != NULL) {
        axl_hash_table_insert(hdrs, "want-digest", axl_strdup("sha-256"));
    }

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "GET", path, hdrs, NULL, 0, &resp);
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
// write_full / create_empty — PUT <path>
// ----------------------------------------------------------------------------

static int
proto_write_full(WebFsPrivate *priv, const char *path,
                 const void *body, size_t len,
                 const char *digest_hex, size_t *out_status)
{
    AxlHashTable *hdrs = NULL;
    if (digest_hex != NULL) {
        hdrs = axl_hash_table_new_str();
        if (hdrs == NULL) return -1;
        char dv[80];
        axl_snprintf(dv, sizeof(dv), "sha-256=%s", digest_hex);
        axl_hash_table_insert(hdrs, "content-digest", axl_strdup(dv));
    }

    /* Stream the body — see the matching JSON impl for the rationale. */
    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request_buf_streaming(
        priv, "PUT", path, hdrs, body, len,
        "application/octet-stream", &resp);

    if (hdrs != NULL) {
        axl_free(axl_hash_table_lookup(hdrs, "content-digest"));
        axl_hash_table_free(hdrs);
    }
    if (rc != 0 || resp == NULL) return -1;
    *out_status = resp->status_code;
    axl_http_client_response_free(resp);
    return 0;
}

static int
proto_create_empty(WebFsPrivate *priv, const char *path, size_t *out_status)
{
    return proto_write_full(priv, path, "", 0, NULL, out_status);
}

// ----------------------------------------------------------------------------
// mkdir — MKCOL <path>
// ----------------------------------------------------------------------------

static int
proto_mkdir(WebFsPrivate *priv, const char *path, size_t *out_status)
{
    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "MKCOL", path, NULL, NULL, 0, &resp);
    if (rc != 0 || resp == NULL) return -1;
    *out_status = resp->status_code;
    axl_http_client_response_free(resp);
    return 0;
}

// ----------------------------------------------------------------------------
// remove — DELETE <path>
// ----------------------------------------------------------------------------

static int
proto_remove(WebFsPrivate *priv, const char *path, size_t *out_status)
{
    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "DELETE", path, NULL, NULL, 0, &resp);
    if (rc != 0) {
        if (resp != NULL) axl_http_client_response_free(resp);
        return -1;
    }
    *out_status = (resp != NULL) ? resp->status_code : 0;
    if (resp != NULL) axl_http_client_response_free(resp);
    return 0;
}

// ----------------------------------------------------------------------------
// rename — MOVE <old> + Destination: <base>/<old_dir>/<new_name>
// ----------------------------------------------------------------------------

static int
proto_rename(WebFsPrivate *priv, const char *old_path,
             const char *new_name, size_t *out_status)
{
    /* Build the Destination header as an absolute URL pointing at
       the same parent directory with the new basename. WebDAV
       servers accept either the full URL or the path-only form;
       wsgidav, Apache mod_dav, and NextCloud all accept full URL
       so that's the safe shape. */
    char old_dir[MAX_PATH_LEN];
    axl_strlcpy(old_dir, old_path, sizeof(old_dir));
    size_t dlen = axl_strlen(old_dir);
    while (dlen > 1 && old_dir[dlen - 1] != '/') {
        old_dir[--dlen] = '\0';
    }

    /* Worst-case bound: base_url (sizeof(priv->base_url)) +
       old_dir (MAX_PATH_LEN) + URL-encoded new_name (3× of the
       256-byte basename → 768) + NUL. Round up; truncation here
       would silently produce a malformed Destination header. */
    char new_name_enc[768];
    axl_url_encode(new_name, new_name_enc, sizeof(new_name_enc));
    char dest_url[sizeof(priv->base_url) + MAX_PATH_LEN
                  + sizeof(new_name_enc) + 1];
    axl_snprintf(dest_url, sizeof(dest_url), "%s%s%s",
                 priv->base_url, old_dir, new_name_enc);

    AxlHashTable *hdrs = axl_hash_table_new_str();
    if (hdrs == NULL) return -1;
    axl_hash_table_insert(hdrs, "destination", axl_strdup(dest_url));
    axl_hash_table_insert(hdrs, "overwrite", axl_strdup("T"));

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "MOVE", old_path, hdrs, NULL, 0, &resp);
    axl_free(axl_hash_table_lookup(hdrs, "destination"));
    axl_free(axl_hash_table_lookup(hdrs, "overwrite"));
    axl_hash_table_free(hdrs);

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

const WebfsProtocolOps webfs_proto_dav = {
    .probe        = proto_probe,
    .list_dir     = proto_list_dir,
    .read_range   = proto_read_range,
    .write_full   = proto_write_full,
    .create_empty = proto_create_empty,
    .mkdir        = proto_mkdir,
    .remove       = proto_remove,
    .rename       = proto_rename,
};
