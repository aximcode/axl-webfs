/** @file
  axl-webfs -- mount service.

  Single-source-file dual-compile (mirrors webfs-serve.c). With
  -DAXL_SERVICE_BUILD_DRIVER this builds into webfs-mount-dxe.efi
  with setup/teardown that bring up the HTTP client and publish a
  filesystem via <axl/axl-fs-provider.h>; the SDK synthesizes the
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL + EFI_FILE_PROTOCOL vtables and
  installs them on a freshly-created handle. Without the define
  only the unconditional bits compile (g_mount_opts, mount_descs,
  the webfs_mount descriptor stub), which is what the launcher
  needs for axl_service_start_embedded's LoadOptions serialization.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "mount/webfs-mount.h"

#include <axl.h>

#ifdef AXL_SERVICE_BUILD_DRIVER
#  include <axl/axl-fs-provider.h>
#  include <axl/axl-net.h>
#  include <axl/axl-net-opts.h>
#  include <axl/axl-url.h>
#  include "mount/webfs-internal.h"
#  include "mount/webfs-protocol.h"
#endif

MountOpts g_mount_opts;

const AxlConfigDesc mount_descs[] = {
    { "url",       AXL_CFG_STRING, "",      "Server URL (http://host[:port][/path])",
      offsetof(MountOpts, url),       sizeof(const char *) },
    { "read-only", AXL_CFG_BOOL,   "false", "Mount read-only",
      offsetof(MountOpts, read_only), sizeof(bool) },
    { "protocol",  AXL_CFG_STRING, "auto",  "Wire protocol: auto, json, dav",
      offsetof(MountOpts, protocol),  sizeof(const char *) },
    { "auth",      AXL_CFG_STRING, "",      "HTTP auth (basic:user:token | bearer:token)",
      offsetof(MountOpts, auth),      sizeof(const char *) },
    /* AXL_NET_OPT_CLIENT preset spelled out: --nic + --source-ip. */
    { "nic",       AXL_CFG_UINT,   AXL_NET_NIC_AUTO_STR,
                                            "NIC index (auto if unset)",
      offsetof(MountOpts, net.nic_index), sizeof(uint64_t) },
    { "source-ip", AXL_CFG_STRING, "",      "IPv4 to bind the outbound socket to (empty = auto)",
      offsetof(MountOpts, net.local_ip),   sizeof(const char *) },
    { 0 }
};

#ifdef AXL_SERVICE_BUILD_DRIVER

/// Vendor GUID for axl-webfs's filesystem-provider device-path node.
/// Used by umount callers (axl-webfs's launcher and Shell users) to
/// recognize the mounted volume's handle.
static const AxlGuid g_webfs_vendor_guid = AXL_GUID(
    0xf47c0fa2, 0xbf67, 0x4c0d,
    0xb0, 0x5e, 0x44, 0x9a, 0x1b, 0xf3, 0x44, 0xc7);

/// Parse "http://1.2.3.4:8080/path" into components using axl_url_parse.
static int
parse_url(
    const char  *url_utf8,
    uint8_t      server_addr[4],
    uint16_t    *port,
    char        *base_path,
    size_t       base_path_size
)
{
    AxlUrl *parsed = NULL;
    if (axl_url_parse(url_utf8, &parsed) != 0)
        return -1;

    uint8_t octets[4];
    if (axl_ipv4_parse(parsed->host, octets) != 0) {
        axl_url_free(parsed);
        return -1;
    }
    server_addr[0] = octets[0];
    server_addr[1] = octets[1];
    server_addr[2] = octets[2];
    server_addr[3] = octets[3];

    *port = (parsed->port != 0) ? parsed->port : DEFAULT_SERVER_PORT;

    const char *url_path = (parsed->path != NULL && parsed->path[0] != '\0')
                           ? parsed->path : "/";
    axl_strlcpy(base_path, url_path, base_path_size);

    size_t path_len = axl_strlen(base_path);
    if (path_len > 0 && base_path[path_len - 1] != '/' &&
        path_len < base_path_size - 1) {
        base_path[path_len] = '/';
        base_path[path_len + 1] = '\0';
    }

    axl_url_free(parsed);
    return 0;
}

static int
mount_setup(AxlLoop *loop, void *user)
{
    (void)loop;  // mount has no loop sources; provider calls are synchronous
    MountOpts *m = user;

    if (m->url == NULL || m->url[0] == '\0') {
        axl_printf("axl-webfs-mount: no URL configured\n");
        return AXL_ERR;
    }

    WebFsPrivate *priv = axl_calloc(1, sizeof(WebFsPrivate));
    if (priv == NULL) {
        return AXL_ERR;
    }
    priv->read_only = m->read_only;

    if (parse_url(m->url, priv->server_addr, &priv->server_port,
                  priv->base_path, sizeof(priv->base_path)) != 0) {
        axl_printf("axl-webfs-mount: invalid URL: %s\n", m->url);
        axl_free(priv);
        return AXL_ERR;
    }

    axl_snprintf(priv->base_url, sizeof(priv->base_url),
                 "http://%d.%d.%d.%d:%d",
                 priv->server_addr[0], priv->server_addr[1],
                 priv->server_addr[2], priv->server_addr[3],
                 priv->server_port);

    axl_printf("axl-webfs-mount: connecting to %d.%d.%d.%d:%d%s\n",
               priv->server_addr[0], priv->server_addr[1],
               priv->server_addr[2], priv->server_addr[3],
               priv->server_port, priv->base_path);

    if (axl_net_init_from_opts(&m->net, 10) != AXL_OK) {
        axl_printf("axl-webfs-mount: network init failed\n");
        axl_free(priv);
        return AXL_ERR;
    }

    priv->http_client = axl_http_client_new();
    if (priv->http_client == NULL) {
        axl_printf("axl-webfs-mount: failed to create HTTP client\n");
        axl_free(priv);
        return AXL_ERR;
    }

    if (m->net.local_ip != NULL && m->net.local_ip[0] != '\0') {
        axl_http_client_set(priv->http_client, "source.ip",
                            m->net.local_ip);
    }

    /* Optional HTTP auth — same parsing as v1. */
    if (m->auth != NULL && m->auth[0] != '\0') {
        char header[1024];
        header[0] = '\0';
        if (axl_strncmp(m->auth, "basic:", 6) == 0) {
            const char *creds = m->auth + 6;
            char *encoded = axl_base64_encode((const uint8_t *)creds,
                                              axl_strlen(creds));
            if (encoded != NULL) {
                axl_snprintf(header, sizeof(header), "Basic %s", encoded);
                axl_free(encoded);
            }
        } else if (axl_strncmp(m->auth, "bearer:", 7) == 0) {
            axl_snprintf(header, sizeof(header), "Bearer %s",
                         m->auth + 7);
        } else {
            axl_printf("axl-webfs-mount: --auth must be "
                       "'basic:user:token' or 'bearer:token'; got '%s'\n",
                       m->auth);
            axl_http_client_free(priv->http_client);
            axl_free(priv);
            return AXL_ERR;
        }
        if (header[0] != '\0') {
            axl_http_client_set(priv->http_client,
                                "header.Authorization", header);
        }
    }

    priv->dir_cache = axl_cache_new(DIR_CACHE_MAX_SLOTS,
                                    sizeof(DirCacheSlot),
                                    DIR_CACHE_TTL_MS);
    if (priv->dir_cache == NULL) {
        axl_http_client_free(priv->http_client);
        axl_free(priv);
        return AXL_ERR;
    }

    /* Wire-protocol probe + override: pick JSON / DAV. */
    const char *proto_cfg = (m->protocol != NULL) ? m->protocol : "auto";
    priv->protocol = WEBFS_PROTO_JSON;
    bool probed_ok = false;

    if (axl_streql(proto_cfg, "dav")) {
        if (webfs_proto_dav.probe(priv) == 0) {
            priv->protocol = WEBFS_PROTO_DAV;
            probed_ok = true;
        }
    } else if (axl_streql(proto_cfg, "json")) {
        if (webfs_proto_json.probe(priv) == 0) {
            priv->protocol = WEBFS_PROTO_JSON;
            probed_ok = true;
        }
    } else {
        if (webfs_proto_dav.probe(priv) == 0) {
            priv->protocol = WEBFS_PROTO_DAV;
            probed_ok = true;
        } else if (webfs_proto_json.probe(priv) == 0) {
            priv->protocol = WEBFS_PROTO_JSON;
            probed_ok = true;
        }
    }

    if (!probed_ok) {
        axl_printf("axl-webfs-mount: server validation failed "
                   "(no %s endpoint reachable)\n", proto_cfg);
        axl_cache_free(priv->dir_cache);
        axl_http_client_free(priv->http_client);
        axl_free(priv);
        return AXL_ERR;
    }

    axl_printf("axl-webfs-mount: wire protocol = %s\n",
               priv->protocol == WEBFS_PROTO_DAV ? "WebDAV" : "JSON");

    /* Publish the filesystem. The provider vtable is statically
       initialized in webfs-file.c; backend_ctx threads the per-mount
       state in. axl_fs_provider_publish synthesizes the EFI vtables,
       allocates the vendor device-path with our GUID, installs both
       protocols on a fresh handle, and connects controllers so Shell
       sees the new fsN: mapping. */
    AxlFsProvider provider = webfs_provider;
    provider.backend_ctx = priv;

    if (axl_fs_provider_publish(&provider, &g_webfs_vendor_guid,
                                &priv->fs_handle) != AXL_OK) {
        axl_printf("axl-webfs-mount: provider publish failed\n");
        axl_cache_free(priv->dir_cache);
        axl_http_client_free(priv->http_client);
        axl_free(priv);
        return AXL_ERR;
    }

    m->priv = priv;
    axl_printf("axl-webfs-mount: mounted (use 'map -r' to refresh "
               "Shell mappings)\n");
    return AXL_OK;
}

static int
mount_teardown(void *user)
{
    MountOpts *m = user;
    WebFsPrivate *priv = m->priv;
    if (priv == NULL) return AXL_OK;

    /* Unpublish first — force-closes any still-open
       AxlFsProviderFile, uninstalls SimpleFs + DevicePath, and
       frees the SDK-side thunk state. Stale UEFI consumer pointers
       get DEVICE_ERROR on next call (per the documented contract). */
    if (priv->fs_handle != NULL) {
        axl_fs_provider_unpublish(priv->fs_handle);
    }
    if (priv->dir_cache != NULL)   axl_cache_free(priv->dir_cache);
    if (priv->http_client != NULL) axl_http_client_free(priv->http_client);
    axl_free(priv);

    m->priv = NULL;
    axl_printf("axl-webfs-mount: unmounted\n");
    return AXL_OK;
}

#endif /* AXL_SERVICE_BUILD_DRIVER */

const AxlService webfs_mount = {
    .name           = "axl-webfs-mount",
    .opts_descs     = mount_descs,
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = mount_setup,
    .teardown       = mount_teardown,
#endif
    .user           = &g_mount_opts,
    /* Mount has no loop sources -- provider calls are synchronous
       from the Shell's caller. The tick still fires; it just iterates
       a sourceless loop. Keep it slow. */
    .driver_tick_ms = 1000,
};

#ifdef AXL_SERVICE_BUILD_DRIVER
AXL_LOG_DOMAIN("webfs-mount");
AXL_SERVICE_DRIVER(webfs_mount);
#endif
