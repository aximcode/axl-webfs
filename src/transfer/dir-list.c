/** @file
  FileTransferLib -- Directory listing as JSON and HTML.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "transfer/file-transfer.h"
#include <axl.h>
#include <axl/axl-json.h>
#include <axl/axl-url.h>


/* Safe snprintf accumulation: clamp pos to prevent size_t underflow */
#define APPEND(pos, buf, sz, ...) do { \
    int _n = axl_snprintf((buf) + (pos), (sz) - (pos), __VA_ARGS__); \
    if (_n > 0) (pos) += (size_t)_n; \
    if ((pos) >= (sz)) (pos) = (sz) - 1; \
} while (0)

// ----------------------------------------------------------------------------
// Path helper (duplicated from FileTransfer.c to keep both files independent)
// ----------------------------------------------------------------------------

static void build_path(const FtVolume *vol, const char *sub_path,
                       char *out, size_t out_size)
{
    axl_snprintf(out, out_size, "%s:%s", vol->name, sub_path);
    for (char *p = out; *p; p++) {
        if (*p == '/')
            *p = '\\';
    }
}

// ----------------------------------------------------------------------------
// HTML escape -- safe for both element-text and double/single-quoted
// attribute contexts. Returns the buffer pointer for inline use in
// APPEND() format-string substitutions. Truncates silently if the
// escaped form exceeds the caller-provided buffer.
// ----------------------------------------------------------------------------

static const char *
html_esc(const char *s, char *buf, size_t buf_size)
{
    if (buf_size == 0)
        return buf;

    size_t pos = 0;
    while (*s != '\0') {
        const char *rep = NULL;
        size_t      rlen = 0;

        switch (*s) {
            case '&':  rep = "&amp;";  rlen = 5; break;
            case '<':  rep = "&lt;";   rlen = 4; break;
            case '>':  rep = "&gt;";   rlen = 4; break;
            case '"':  rep = "&quot;"; rlen = 6; break;
            case '\'': rep = "&#39;";  rlen = 5; break;
            default:   break;
        }

        if (rep != NULL) {
            if (pos + rlen >= buf_size)
                break;
            for (size_t i = 0; i < rlen; i++)
                buf[pos++] = rep[i];
        } else {
            if (pos + 1 >= buf_size)
                break;
            buf[pos++] = *s;
        }

        s++;
    }

    buf[pos] = '\0';
    return buf;
}

// ----------------------------------------------------------------------------
// Volume listing
// ----------------------------------------------------------------------------

int ft_list_volumes(bool as_json, char *buf, size_t buf_size, size_t *written)
{
    size_t pos = 0;
    size_t count = ft_get_volume_count();

    if (as_json) {
        APPEND(pos, buf, buf_size, "[");
        for (size_t i = 0; i < count; i++) {
            FtVolume vol;
            ft_get_volume(i, &vol);
            if (i > 0)
                APPEND(pos, buf, buf_size, ",");
            APPEND(pos, buf, buf_size,
                "{\"name\":\"%s\",\"index\":%zu}", vol.name, i);
        }
        APPEND(pos, buf, buf_size, "]");
    } else {
        APPEND(pos, buf, buf_size,
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>axl-webfs &mdash; Volumes</title>"
            "<style>"
            "body{background:#1a1a2e;color:#e0e0e0;font-family:-apple-system,"
            "BlinkMacSystemFont,\"Segoe UI\",Roboto,monospace;margin:0;padding:2em}"
            "a{color:#4a9eff;text-decoration:none}a:hover{color:#6ab4ff}"
            "h2{color:#4a9eff;margin-bottom:.3em}"
            ".crumb{color:#8888aa;margin-bottom:1.5em;font-size:.9em}"
            ".card{background:#16213e;border:1px solid #0f3460;border-radius:8px;"
            "padding:1.5em;max-width:900px}"
            "table{border-collapse:collapse;width:100%%}"
            "th{text-align:left;padding:8px 12px;border-bottom:2px solid #0f3460;"
            "color:#8888aa;font-size:.85em;text-transform:uppercase}"
            "td{padding:8px 12px;border-bottom:1px solid #0f3460}"
            "tr:hover{background:#1a4a80}"
            "</style></head><body>"
            "<h2>axl-webfs</h2>"
            "<div class=\"crumb\">Volumes</div>"
            "<div class=\"card\"><table>"
            "<tr><th>Volume</th></tr>");

        for (size_t i = 0; i < count; i++) {
            FtVolume vol;
            ft_get_volume(i, &vol);
            char vol_esc[128];
            html_esc(vol.name, vol_esc, sizeof(vol_esc));
            APPEND(pos, buf, buf_size,
                "<tr><td><a href=\"/%s/\">%s:</a></td></tr>",
                vol_esc, vol_esc);
        }

        APPEND(pos, buf, buf_size,
            "</table></div></body></html>");
    }

    *written = pos;
    return 0;
}

// ----------------------------------------------------------------------------
// Directory listing
// ----------------------------------------------------------------------------

int ft_list_dir(FtVolume *vol, const char *path, bool as_json, bool read_only,
                char *buf, size_t buf_size, size_t *written)
{
    char dir_full[512];
    build_path(vol, path, dir_full, sizeof(dir_full));

    AxlDir *dir = axl_dir_open(dir_full);
    if (dir == NULL)
        return -1;

    size_t pos = 0;
    bool first = true;

    if (as_json) {
        APPEND(pos, buf, buf_size, "[");
    } else {
        char vol_esc[128];
        char path_esc[3072];                  /* path is bounded at 512 raw chars */
        html_esc(vol->name, vol_esc, sizeof(vol_esc));
        html_esc(path,      path_esc, sizeof(path_esc));

        APPEND(pos, buf, buf_size,
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>%s:%s</title>",
            vol_esc, path_esc);

        APPEND(pos, buf, buf_size,
            "<style>"
            "body{background:#1a1a2e;color:#e0e0e0;font-family:-apple-system,"
            "BlinkMacSystemFont,\"Segoe UI\",Roboto,monospace;margin:0;padding:2em}"
            "a{color:#4a9eff;text-decoration:none}a:hover{color:#6ab4ff}"
            "h2{color:#4a9eff;margin-bottom:.3em}"
            ".crumb{color:#8888aa;margin-bottom:1.5em;font-size:.9em}"
            ".crumb a{color:#4a9eff}"
            ".card{background:#16213e;border:1px solid #0f3460;border-radius:8px;"
            "padding:1.5em;max-width:900px}"
            "table{border-collapse:collapse;width:100%%}"
            "th{text-align:left;padding:8px 12px;border-bottom:2px solid #0f3460;"
            "color:#8888aa;font-size:.85em;text-transform:uppercase}"
            "td{padding:8px 12px;border-bottom:1px solid #0f3460}"
            "tr:hover{background:#1a4a80}"
            ".sz{text-align:right;color:#8888aa;white-space:nowrap}"
            ".ty{color:#8888aa}"
            "[hidden]{display:none}");

        if (!read_only) {
            APPEND(pos, buf, buf_size,
                ".card{transition:background .15s,outline .15s}"
                ".card.dropzone-active{outline:2px dashed #4a9eff;"
                "outline-offset:-6px;background:#1a4a80}"
                ".actions{display:flex;justify-content:flex-end;margin-bottom:1em}"
                ".upload-btn{background:#0f3460;color:#e0e0e0;"
                "border:1px solid #4a9eff;border-radius:6px;padding:8px 16px;"
                "cursor:pointer;font-family:inherit;font-size:.9em}"
                ".upload-btn:hover{background:#1a4a80;color:#fff}"
                ".queue{position:fixed;bottom:1em;right:1em;width:320px;"
                "max-height:50vh;overflow:auto;background:#16213e;"
                "border:1px solid #0f3460;border-radius:8px;padding:.75em;"
                "box-shadow:0 4px 12px rgba(0,0,0,.5)}"
                ".queue-row{padding:.4em 0}"
                ".queue-row+.queue-row{border-top:1px solid #0f3460}"
                ".queue-name{display:block;font-size:.85em;margin-bottom:4px;"
                "word-break:break-all}"
                ".queue-row progress{width:100%%;height:6px;border:none;"
                "background:#0f3460;border-radius:3px}"
                ".queue-row progress::-webkit-progress-bar"
                "{background:#0f3460;border-radius:3px}"
                ".queue-row progress::-webkit-progress-value"
                "{background:#4a9eff;border-radius:3px}"
                ".queue-row progress::-moz-progress-bar"
                "{background:#4a9eff;border-radius:3px}"
                ".queue-row.err{color:#ff6464}"
                ".queue-row.err progress::-webkit-progress-value"
                "{background:#ff6464}"
                ".queue-row.err progress::-moz-progress-bar"
                "{background:#ff6464}");
        }

        APPEND(pos, buf, buf_size,
            "</style></head><body>"
            "<h2>axl-webfs</h2>");

        /* Breadcrumb: Volumes / fs0: / path / subdir */
        APPEND(pos, buf, buf_size,
            "<div class=\"crumb\"><a href=\"/\">Volumes</a>"
            " / <a href=\"/%s/\">%s:</a>", vol_esc, vol_esc);

        /* Add path segments as breadcrumb links */
        {
            char href[512];
            int href_len = axl_snprintf(href, sizeof(href), "/%s", vol->name);
            char seg[256];
            size_t si = 0;
            size_t pi = 0;
            if (path[0] == '/')
                pi = 1;

            while (path[pi] != '\0') {
                if (path[pi] == '/') {
                    seg[si] = '\0';
                    if (si > 0) {
                        href_len += axl_snprintf(href + href_len,
                            sizeof(href) - (size_t)href_len, "/%s", seg);
                        char href_esc[1536];
                        char seg_esc[1536];
                        html_esc(href, href_esc, sizeof(href_esc));
                        html_esc(seg,  seg_esc,  sizeof(seg_esc));
                        APPEND(pos, buf, buf_size,
                            " / <a href=\"%s/\">%s</a>", href_esc, seg_esc);
                    }
                    si = 0;
                } else {
                    if (si < 255)
                        seg[si++] = path[pi];
                }
                pi++;
            }
            /* Last segment (current directory -- not a link) */
            if (si > 0) {
                seg[si] = '\0';
                char seg_esc[1536];
                html_esc(seg, seg_esc, sizeof(seg_esc));
                APPEND(pos, buf, buf_size, " / %s", seg_esc);
            }
        }

        APPEND(pos, buf, buf_size, "</div>");

        APPEND(pos, buf, buf_size, "<div class=\"card\">");

        if (!read_only) {
            APPEND(pos, buf, buf_size,
                "<div class=\"actions\">"
                "<button class=\"upload-btn\" id=\"axl-upload-btn\">Upload</button>"
                "<input type=\"file\" id=\"axl-upload-input\" multiple hidden>"
                "</div>");
        }

        APPEND(pos, buf, buf_size,
            "<table>"
            "<tr><th>Name</th><th class=\"sz\">Size</th><th>Type</th></tr>");

        /* ".." parent directory link */
        {
            char parent_href[512];
            size_t pi = 0;
            while (path[pi] != '\0' && pi < 511) {
                parent_href[pi] = path[pi];
                pi++;
            }
            parent_href[pi] = '\0';
            /* Remove trailing slash */
            if (pi > 0 && parent_href[pi - 1] == '/')
                parent_href[--pi] = '\0';
            /* Find last slash */
            int last = (int)pi - 1;
            while (last >= 0 && parent_href[last] != '/')
                last--;
            if (last >= 0) {
                parent_href[last + 1] = '\0';
                char parent_href_esc[1536];
                html_esc(parent_href, parent_href_esc, sizeof(parent_href_esc));
                APPEND(pos, buf, buf_size,
                    "<tr><td><a href=\"/%s%s\">..</a></td>"
                    "<td class=\"sz\">&mdash;</td>"
                    "<td class=\"ty\">dir</td></tr>",
                    vol_esc, parent_href_esc);
            } else {
                /* At volume root -- parent is volume list */
                APPEND(pos, buf, buf_size,
                    "<tr><td><a href=\"/\">..</a></td>"
                    "<td class=\"sz\">&mdash;</td>"
                    "<td class=\"ty\">dir</td></tr>");
            }
        }
    }

    /* Read directory entries */
    AxlDirEntry entry;
    while (axl_dir_read(dir, &entry)) {
        if (axl_streql(entry.name, ".") || axl_streql(entry.name, ".."))
            continue;

        if (as_json) {
            if (!first)
                APPEND(pos, buf, buf_size, ",");
            first = false;
            char esc_name[512];
            axl_json_escape_string(entry.name, esc_name, sizeof(esc_name));
            APPEND(pos, buf, buf_size,
                "{\"name\":%s,\"size\":%llu,\"dir\":%s}",
                esc_name, (unsigned long long)entry.size,
                entry.is_dir ? "true" : "false");
        } else {
            char url_name[512];
            char name_esc[1536];
            axl_url_encode(entry.name, url_name, sizeof(url_name));
            html_esc(entry.name, name_esc, sizeof(name_esc));
            if (entry.is_dir) {
                APPEND(pos, buf, buf_size,
                    "<tr><td><a href=\"%s/\">%s/</a></td>"
                    "<td class=\"sz\">&mdash;</td>"
                    "<td class=\"ty\">dir</td></tr>",
                    url_name, name_esc);
            } else {
                APPEND(pos, buf, buf_size,
                    "<tr><td><a href=\"%s\">%s</a></td>"
                    "<td class=\"sz\">%llu</td>"
                    "<td class=\"ty\">file</td></tr>",
                    url_name, name_esc,
                    (unsigned long long)entry.size);
            }
        }

        if (pos >= buf_size - 256)
            break;  /* Safety margin */
    }

    if (as_json) {
        APPEND(pos, buf, buf_size, "]");
    } else {
        APPEND(pos, buf, buf_size, "</table></div>");
        if (!read_only) {
            APPEND(pos, buf, buf_size,
                "<div class=\"queue\" id=\"axl-queue\" hidden></div>"
                "<script src=\"/_axl-webfs/upload.js\"></script>");
        }
        APPEND(pos, buf, buf_size, "</body></html>");
    }

    axl_dir_close(dir);
    *written = pos;
    return 0;
}
