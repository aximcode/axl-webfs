/** @file
  FileTransferLib -- Directory listing as JSON and HTML.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/FileTransferLib.h>
#include <axl.h>
#include <axl/axl-json.h>


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
            "<title>HttpFS &mdash; Volumes</title>"
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
            "<h2>HttpFS</h2>"
            "<div class=\"crumb\">Volumes</div>"
            "<div class=\"card\"><table>"
            "<tr><th>Volume</th></tr>");

        for (size_t i = 0; i < count; i++) {
            FtVolume vol;
            ft_get_volume(i, &vol);
            APPEND(pos, buf, buf_size,
                "<tr><td><a href=\"/%s/\">%s:</a></td></tr>", vol.name, vol.name);
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

int ft_list_dir(FtVolume *vol, const char *path, bool as_json,
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
        APPEND(pos, buf, buf_size,
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>%s:%s</title>"
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
            "</style></head><body>"
            "<h2>HttpFS</h2>",
            vol->name, path);

        /* Breadcrumb: Volumes / fs0: / path / subdir */
        APPEND(pos, buf, buf_size,
            "<div class=\"crumb\"><a href=\"/\">Volumes</a>"
            " / <a href=\"/%s/\">%s:</a>", vol->name, vol->name);

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
                        APPEND(pos, buf, buf_size,
                            " / <a href=\"%s/\">%s</a>", href, seg);
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
                APPEND(pos, buf, buf_size,
                    " / %s", seg);
            }
        }

        APPEND(pos, buf, buf_size, "</div>");

        APPEND(pos, buf, buf_size,
            "<div class=\"card\"><table>"
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
                APPEND(pos, buf, buf_size,
                    "<tr><td><a href=\"/%s%s\">..</a></td>"
                    "<td class=\"sz\">&mdash;</td>"
                    "<td class=\"ty\">dir</td></tr>",
                    vol->name, parent_href);
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
            APPEND(pos, buf, buf_size,
                "{\"name\":\"%s\",\"size\":%llu,\"dir\":%s}",
                entry.name, (unsigned long long)entry.size,
                entry.is_dir ? "true" : "false");
        } else {
            if (entry.is_dir) {
                APPEND(pos, buf, buf_size,
                    "<tr><td><a href=\"%s/\">%s/</a></td>"
                    "<td class=\"sz\">&mdash;</td>"
                    "<td class=\"ty\">dir</td></tr>",
                    entry.name, entry.name);
            } else {
                APPEND(pos, buf, buf_size,
                    "<tr><td><a href=\"%s\">%s</a></td>"
                    "<td class=\"sz\">%llu</td>"
                    "<td class=\"ty\">file</td></tr>",
                    entry.name, entry.name,
                    (unsigned long long)entry.size);
            }
        }

        if (pos >= buf_size - 256)
            break;  /* Safety margin */
    }

    if (as_json) {
        APPEND(pos, buf, buf_size, "]");
    } else {
        APPEND(pos, buf, buf_size,
            "</table></div></body></html>");
    }

    axl_dir_close(dir);
    *written = pos;
    return 0;
}
