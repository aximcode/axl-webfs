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
#include <axl/axl-string.h>
#include <axl/axl-xml.h>

AXL_LOG_DOMAIN("webfs-dav-client");

// ----------------------------------------------------------------------------
// Path helpers
// ----------------------------------------------------------------------------

/* What an href is to the listing of one directory. */
typedef enum {
    HREF_ENTRY,      ///< a path in the share; DavEntry.href holds it
    HREF_FOREIGN,    ///< outside the share (or climbs out of it): no entry
    HREF_TOO_LONG,   ///< its mount path does not fit MAX_PATH_LEN: skipped
    HREF_BAD,        ///< cannot be read (bad escape, ".." above the root)
} HrefKind;

/// Whether @p href begins with a URI scheme ("http:", "HTTP:", ...):
/// ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" (RFC 3986 3.1).
static bool
href_has_scheme(const char *href)
{
    const char *q = href;
    if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')))
        return false;
    for (q++; *q != '\0' && *q != ':'; q++) {
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
              (*q >= '0' && *q <= '9') || *q == '+' || *q == '-' || *q == '.'))
            return false;
    }
    return *q == ':';
}

/// Turn a PROPFIND <D:href> into a mount path ("/" + share-relative) in
/// @p out.
///   - An absolute URI (a scheme, in any case) or a network-path
///     reference ("//host/..", RFC 3986 4.2) is parsed with
///     axl_url_parse, and is ours only when its scheme is http and its
///     host and EFFECTIVE port are the mount's: another host or port is
///     HREF_FOREIGN, an href that does not parse HREF_BAD. Its query and
///     fragment are no part of the path.
///   - A relative path reference resolves against @p req_dir, the
///     decoded directory the PROPFIND was sent to (RFC 3986 5.2):
///     "rel.txt" in a listing of "/my share/" is "/my share/rel.txt".
/// The path is percent-decoded -- a %00 (the decode's length is not the
/// string's) is HREF_BAD -- and normalized ("." and "..") BEFORE it is
/// matched against the share's base, so "<base>/../x" is the sibling it
/// names, outside the share, never a child "x".
static HrefKind
href_to_mount_path(const WebFsPrivate *priv, const char *req_dir,
                   const char *href, char *out, size_t out_size)
{
    char   *raw  = NULL;    /* the path part, still encoded */
    HrefKind kind = HREF_BAD;

    if (href_has_scheme(href) || (href[0] == '/' && href[1] == '/')) {
        char   *abs = NULL;
        AxlUrl *u   = NULL;
        if (href[0] == '/') {
            /* A network-path reference takes the request's scheme. */
            abs = axl_asprintf("http:%s", href);
            if (abs == NULL)
                return HREF_BAD;
        }
        if (axl_url_parse(abs != NULL ? abs : href, &u) != AXL_OK) {
            axl_free(abs);
            return HREF_BAD;
        }
        axl_free(abs);
        char host[16];
        axl_snprintf(host, sizeof(host), "%d.%d.%d.%d",
                     priv->server_addr[0], priv->server_addr[1],
                     priv->server_addr[2], priv->server_addr[3]);
        /* axl_url_parse stores the scheme lowercased (schemes are
           case-insensitive, RFC 3986 3.1) and fills the scheme's default
           port when none is given: HTTP://host/ is http on port 80. */
        if (!axl_streql(u->scheme, "http") ||
            axl_strcasecmp(u->host, host) != 0 ||
            u->port != priv->server_port) {
            axl_url_free(u);
            return HREF_FOREIGN;
        }
        raw = axl_strdup(u->path != NULL ? u->path : "/");
        axl_url_free(u);
    } else {
        raw = axl_strdup(href);
    }
    if (raw == NULL)
        return HREF_BAD;

    size_t cap = axl_strlen(raw) + 1;
    char  *dec = axl_malloc(cap);
    char  *res = axl_malloc(MAX_PATH_LEN * 4);
    int    n   = dec != NULL ? axl_url_decode(raw, dec, cap) : -1;
    if (n >= 0 && axl_strlen(dec) == (size_t)n && res != NULL &&
        axl_path_resolve(req_dir, dec, res, MAX_PATH_LEN * 4) == AXL_OK) {
        const char *base = priv->base_path;
        size_t      blen = axl_strlen(base);
        const char *rel  = res;
        if (blen > 0) {
            if (axl_strncmp(res, base, blen) != 0 ||
                (res[blen] != '\0' && res[blen] != '/'))
                rel = NULL;
            else
                rel = res + blen;
        }
        if (rel == NULL)
            kind = HREF_FOREIGN;
        else if (axl_strlcpy(out, rel[0] != '\0' ? rel : "/", out_size) >= out_size)
            kind = HREF_TOO_LONG;
        else
            kind = HREF_ENTRY;
    }
    axl_free(raw);
    axl_free(dec);
    axl_free(res);
    return kind;
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

/* The PROPFIND body asks for "allprop" -- works against every
   WebDAV server we care about (wsgidav, Apache mod_dav, NextCloud,
   IIS). Strict servers that return more verbose listings just give
   us more fields to ignore; pull-token parsing is robust to that. */
static const char k_propfind_allprop[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:allprop/></D:propfind>";

// ----------------------------------------------------------------------------
// probe — OPTIONS / and check for DAV: header
// ----------------------------------------------------------------------------

static int
proto_probe(WebFsPrivate *priv)
{
    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "OPTIONS", "/", NULL, NULL, NULL, 0, &resp);
    if (rc != 0 || resp == NULL) return -1;

    int ok = 0;
    if (resp->status_code >= 200 && resp->status_code < 400 &&
        resp->headers != NULL) {
        const char *dav = axl_hash_table_lookup(resp->headers, "dav");
        if (dav != NULL && dav[0] != '\0') ok = 1;
    }
    axl_http_client_response_free(resp);
    if (!ok)
        return -1;

    /* OPTIONS answers for any path; the share must EXIST as a collection
       (PROPFIND Depth: 0 of its root is 207), or every listing of it
       would fail and the Shell show the volume empty. */
    AxlHashTable *hdrs = axl_hash_table_new_str();
    if (hdrs == NULL) return -1;
    axl_hash_table_insert(hdrs, "depth", axl_strdup("0"));
    resp = NULL;
    rc = webfs_http_request(priv, "PROPFIND", "/", NULL, hdrs,
                            k_propfind_allprop, sizeof(k_propfind_allprop) - 1,
                            &resp);
    axl_free(axl_hash_table_lookup(hdrs, "depth"));
    axl_hash_table_free(hdrs);
    ok = rc == 0 && resp != NULL && resp->status_code == 207;
    if (resp != NULL) axl_http_client_response_free(resp);
    if (!ok)
        axl_printf("axl-webfs-mount: the share is no WebDAV collection on "
                   "the server\n");
    return ok ? 0 : -1;
}

// ----------------------------------------------------------------------------
// list_dir — PROPFIND Depth: 1
// ----------------------------------------------------------------------------


/// PROPFIND parser state: one <D:response> at a time, accumulating
/// the four fields we need before the matching </D:response> closes.
typedef struct {
    char     href[MAX_PATH_LEN];
    char     name[WEBFS_NAME_SIZE];
    uint64_t size;
    bool     is_collection;
    bool     have_href;
    bool     too_long;          ///< href or displayname did not fit: skip
    bool     foreign;           ///< href outside the share: no entry
    bool     bad;               ///< href unreadable: the listing fails
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
               DirEntry *out, bool *is_self)
{
    *is_self = false;
    if (!e->have_href) return false;
    if (e->foreign) {
        axl_warning("listing %s: skipping an href outside the share",
                    parent_path);
        return false;
    }
    if (e->too_long) {
        axl_warning("listing %s: skipping an entry whose name or href does "
                    "not fit (names up to %zu bytes)", parent_path,
                    sizeof(out->name) - 1);
        return false;
    }

    /* The self entry -- PROPFIND Depth:1 returns the queried resource
       plus its children -- is the href that resolves to parent_path
       itself (e->href is normalized, with no trailing separator; and
       parent_path is the mount's own raw path: decoding it again would
       turn a directory named "100%41" into "100A"). Every other href
       must be a DIRECT child of parent_path; one that is not (a
       grandchild, a sibling) is no entry of this listing, never shown
       under its basename. */
    size_t hlen = axl_strlen(e->href);
    size_t plen = axl_strlen(parent_path);
    while (plen > 1 && parent_path[plen - 1] == '/') plen--;
    if (hlen == plen && axl_memcmp(e->href, parent_path, hlen) == 0) {
        *is_self = true;
        return false;
    }
    const char *slash = e->href;
    for (const char *q = e->href; *q != '\0'; q++)
        if (*q == '/') slash = q;
    size_t dlen = (size_t)(slash - e->href);
    bool   child = (plen == 1 && parent_path[0] == '/')
                   ? dlen == 0
                   : (dlen == plen && axl_memcmp(e->href, parent_path, plen) == 0);
    if (!child || slash[1] == '\0') {
        axl_warning("listing %s: skipping an href that is no child of it",
                    parent_path);
        return false;
    }

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
    /* The collection is asked for with its trailing separator, and its
       decoded server path is what a relative href resolves against
       (RFC 3986 5.2): "x" in the answer for "/my share/sub/" is
       "/my share/sub/x". */
    char   dir_path[MAX_PATH_LEN + 1];
    char   req_dir[sizeof(priv->base_path) + sizeof(dir_path)];
    size_t plen  = axl_strlen(path);
    bool   slash = plen > 0 && path[plen - 1] == '/';
    int    n1    = axl_snprintf(dir_path, sizeof(dir_path), "%s%s", path,
                                slash ? "" : "/");
    int    n2    = axl_snprintf(req_dir, sizeof(req_dir), "%s%s",
                                priv->base_path, dir_path);
    if (n1 < 0 || (size_t)n1 >= sizeof(dir_path) ||
        n2 < 0 || (size_t)n2 >= sizeof(req_dir))
        return -1;

    AxlHashTable *hdrs = axl_hash_table_new_str();
    if (hdrs == NULL) return -1;
    axl_hash_table_insert(hdrs, "depth", axl_strdup("1"));
    axl_hash_table_insert(hdrs, "content-type",
                          axl_strdup("application/xml; charset=\"utf-8\""));

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "PROPFIND", dir_path, NULL, hdrs,
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
    bool   failed = false;   /* over the cap, out of memory, a bad href */
    bool   saw_self = false;
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
    while (!failed && axl_xml_reader_next(r, &tok)) {
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
                /* Out of memory fails the LISTING: skipping the entry
                   would hand back a short listing as the whole one. */
                if (axl_string_append_len(text, tok.text, tok.text_len)
                        != AXL_OK)
                    failed = true;
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
                switch (href_to_mount_path(priv, req_dir,
                                           axl_string_str(text), cur.href,
                                           sizeof(cur.href))) {
                    case HREF_ENTRY:    break;
                    case HREF_FOREIGN:  cur.foreign  = true; break;
                    case HREF_TOO_LONG: cur.too_long = true; break;
                    default:            cur.bad      = true; break;
                }
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
                bool     self = false;
                if (cur.bad) {
                    axl_error("listing %s: an href cannot be read; refused",
                              path);
                    failed = true;   /* never a short listing */
                } else if (dav_entry_emit(&cur, path, &entry, &self) &&
                           webfs_listing_append(out, &entry, path) != 0) {
                    failed = true;   /* never a short listing */
                }
                saw_self = saw_self || self;
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
    /* Without the listed collection itself among the hrefs, nothing says
       the rest are its children: the share the hrefs are read against is
       not where the server answered. An empty or short listing would pass
       for the directory; fail it instead. */
    if (!failed && !saw_self) {
        axl_error("listing %s: the server's answer has no entry for the "
                  "listed collection itself; refused", path);
        return -1;
    }

    return failed ? -1 : 0;
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
    int rc = webfs_http_request(priv, "GET", path, NULL, hdrs, NULL, 0, &resp);
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
    int rc = webfs_http_request(priv, "MKCOL", path, NULL, NULL, NULL, 0, &resp);
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
    int rc = webfs_http_request(priv, "DELETE", path, NULL, NULL, NULL, 0, &resp);
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
    /* The new path is the old one's directory plus the new name, and
       the URL is built -- the WHOLE path percent-encoded -- exactly as
       a request URL is: a directory named with a space, '%' or a
       non-ASCII byte must reach the server as that name. */
    char   new_path[MAX_PATH_LEN];
    size_t dlen = axl_strlen(old_path);
    while (dlen > 1 && old_path[dlen - 1] != '/')
        dlen--;
    int n = axl_snprintf(new_path, sizeof(new_path), "%.*s%s",
                         (int)dlen, old_path, new_name);
    char dest_url[WEBFS_URL_MAX];
    if (n < 0 || (size_t)n >= sizeof(new_path) ||
        !webfs_request_url(priv, new_path, NULL, dest_url, sizeof(dest_url)))
        return -1;

    AxlHashTable *hdrs = axl_hash_table_new_str();
    if (hdrs == NULL) return -1;
    axl_hash_table_insert(hdrs, "destination", axl_strdup(dest_url));
    axl_hash_table_insert(hdrs, "overwrite", axl_strdup("T"));

    AxlHttpClientResponse *resp = NULL;
    int rc = webfs_http_request(priv, "MOVE", old_path, NULL, hdrs, NULL, 0, &resp);
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
