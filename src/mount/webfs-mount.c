/** @file
  axl-webfs -- mount service.

  Single-source-file dual-compile (mirrors webfs-serve.c). With
  -DAXL_SERVICE_BUILD_DRIVER this builds into webfs-mount-dxe.efi
  with setup/teardown that bring up the HTTP client, install
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on a new handle (vendor device
  path so umount can find it), and reverse the dance on unload.
  Without the define only the unconditional bits compile
  (g_mount_opts, mount_descs, the webfs_mount descriptor stub),
  which is what the launcher needs for axl_service_start_embedded's
  LoadOptions serialization.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "mount/webfs-mount.h"

#include <axl.h>

#ifdef AXL_SERVICE_BUILD_DRIVER
#  include <axl/axl-net.h>
#  include <axl/axl-url.h>
#  include "mount/webfs-internal.h"
#  include "net/network.h"
#endif

MountOpts g_mount_opts;

const AxlConfigDesc mount_descs[] = {
    { "url",       AXL_CFG_STRING, "",      "Server URL (http://host[:port][/path])",
      offsetof(MountOpts, url),       sizeof(const char *) },
    { "read-only", AXL_CFG_BOOL,   "false", "Mount read-only",
      offsetof(MountOpts, read_only), sizeof(bool) },
    { 0 }
};

#ifdef AXL_SERVICE_BUILD_DRIVER

/// Vendor GUID for the axl-webfs device path node — used by umount
/// callers (axl-webfs's launcher and Shell users) to recognize the
/// mounted volume's handle.
static const EFI_GUID HttpFsVendorGuid = {
    0xf47c0fa2, 0xbf67, 0x4c0d,
    {0xb0, 0x5e, 0x44, 0x9a, 0x1b, 0xf3, 0x44, 0xc7}
};

#pragma pack(1)
typedef struct {
    VENDOR_DEVICE_PATH       Vendor;
    EFI_DEVICE_PATH_PROTOCOL End;
} WebFsDevicePath;
#pragma pack()

static const WebFsDevicePath m_device_path_template = {
    {
        { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
          { sizeof(VENDOR_DEVICE_PATH), 0 } },
        { 0 }  // GUID filled at runtime
    },
    { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
      { sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

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

EFI_STATUS
EFIAPI
WebFsOpenVolume(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL              **Root
)
{
    WebFsPrivate *priv = WEBFS_PRIVATE_FROM_SIMPLE_FS(This);

    WebFsFileCtx *root_file = webfs_create_file_handle(
        priv, priv->base_path, true, 0);
    if (root_file == NULL) return EFI_OUT_OF_RESOURCES;

    root_file->is_root = true;
    *Root = &root_file->file;
    return EFI_SUCCESS;
}

static int
mount_setup(AxlLoop *loop, void *user)
{
    (void)loop;  // mount has no loop sources; protocol calls are synchronous
    MountOpts *m = user;

    if (m->url == NULL || m->url[0] == '\0') {
        axl_printf("axl-webfs-mount: no URL configured\n");
        return AXL_ERR;
    }

    WebFsPrivate *priv = axl_calloc(1, sizeof(WebFsPrivate));
    if (priv == NULL) {
        return AXL_ERR;
    }
    priv->signature = WEBFS_PRIVATE_SIGNATURE;
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

    if (network_init((size_t)-1, NULL, 10) != 0) {
        axl_printf("axl-webfs-mount: network init failed\n");
        axl_free(priv);
        return AXL_ERR;
    }

    priv->http_client = axl_http_client_new();
    if (priv->http_client == NULL) {
        axl_printf("axl-webfs-mount: failed to create HTTP client\n");
        network_cleanup();
        axl_free(priv);
        return AXL_ERR;
    }

    /* Validate server with GET /info before publishing the protocol
       handle (so a misconfigured URL doesn't leave a broken FSn:
       lying around). */
    char info_url[320];
    axl_snprintf(info_url, sizeof(info_url), "%s/info", priv->base_url);
    AxlHttpClientResponse *info_resp = NULL;
    int info_ret = axl_http_get(priv->http_client, info_url, &info_resp);
    if (info_ret != 0 || info_resp == NULL || info_resp->status_code != 200) {
        axl_printf("axl-webfs-mount: server validation failed\n");
        if (info_resp != NULL) axl_http_client_response_free(info_resp);
        axl_http_client_free(priv->http_client);
        network_cleanup();
        axl_free(priv);
        return AXL_ERR;
    }
    axl_http_client_response_free(info_resp);

    priv->simple_fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    priv->simple_fs.OpenVolume = WebFsOpenVolume;

    WebFsDevicePath *dev_path = axl_malloc(sizeof(WebFsDevicePath));
    if (dev_path == NULL) {
        axl_http_client_free(priv->http_client);
        network_cleanup();
        axl_free(priv);
        return AXL_ERR;
    }
    axl_memcpy(dev_path, &m_device_path_template, sizeof(WebFsDevicePath));
    axl_memcpy(&dev_path->Vendor.Guid, &HttpFsVendorGuid, sizeof(EFI_GUID));
    priv->device_path = (EFI_DEVICE_PATH_PROTOCOL *)dev_path;

    priv->fs_handle = NULL;
    if (axl_protocol_register_multiple(&priv->fs_handle,
            "simple-fs", &priv->simple_fs,
            "device-path", priv->device_path,
            NULL) != 0) {
        axl_printf("axl-webfs-mount: protocol install failed\n");
        axl_free(dev_path);
        axl_http_client_free(priv->http_client);
        network_cleanup();
        axl_free(priv);
        return AXL_ERR;
    }

    /* Connect controllers so Shell sees the new FS mapping
       without `map -r`. */
    axl_driver_connect_handle(priv->fs_handle);

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
    if (priv == NULL) {
        return AXL_OK;
    }

    if (priv->fs_handle != NULL) {
        axl_protocol_unregister(priv->fs_handle, "simple-fs", &priv->simple_fs);
        axl_protocol_unregister(priv->fs_handle, "device-path", priv->device_path);
    }
    if (priv->http_client != NULL) {
        axl_http_client_free(priv->http_client);
    }
    if (priv->device_path != NULL) {
        axl_free(priv->device_path);
    }
    axl_free(priv);
    network_cleanup();

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
    /* Mount has no loop sources -- EFI_FILE_PROTOCOL calls are
       synchronous from the Shell's caller. The tick still fires;
       it just iterates a sourceless loop. Keep it slow. */
    .driver_tick_ms = 1000,
};

#ifdef AXL_SERVICE_BUILD_DRIVER
AXL_LOG_DOMAIN("webfs-mount");
AXL_SERVICE_DRIVER(webfs_mount);
#endif
