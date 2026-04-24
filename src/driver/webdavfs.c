/** @file
  WebDavFsDxe -- Driver entry point, URL parsing, protocol install.

  Loads as a UEFI driver, parses the server URL from load options,
  establishes an HTTP connection, and installs EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
  on a new device handle so the UEFI Shell sees it as FSn:.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webdavfs-internal.h"

/// Vendor GUID for the WebDavFs device path node.
static const EFI_GUID HttpFsVendorGuid = {
    0xf47c0fa2, 0xbf67, 0x4c0d,
    {0xb0, 0x5e, 0x44, 0x9a, 0x1b, 0xf3, 0x44, 0xc7}
};

/// Module-global pointer to the driver private context (for unload path).
static WebDavFsPrivate *mPrivate = NULL;

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

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

    // Parse IPv4 address from host string
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
    if (path_len > 0 && base_path[path_len - 1] != '/' && path_len < base_path_size - 1) {
        base_path[path_len] = '/';
        base_path[path_len + 1] = '\0';
    }

    axl_url_free(parsed);
    return 0;
}

// ---------------------------------------------------------------------------
// Vendor device path for the mounted volume
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    VENDOR_DEVICE_PATH       Vendor;
    EFI_DEVICE_PATH_PROTOCOL End;
} WebDavFsDevicePath;
#pragma pack()

static WebDavFsDevicePath mDevicePathTemplate = {
    {
        { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
          { sizeof(VENDOR_DEVICE_PATH), 0 } },
        { 0 }  // GUID filled at runtime
    },
    { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
      { sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

// ---------------------------------------------------------------------------
// Driver entry and unload
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
DriverEntry(
    EFI_HANDLE        ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
)
{
    // Initialize AXL runtime (sets gST/gBS/gRT, enables axl_printf)
    axl_driver_init(ImageHandle, SystemTable);

    // Get URL from load options (UTF-8, allocated by SDK)
    char *url_utf8 = axl_driver_get_load_options();
    if (url_utf8 == NULL) {
        axl_printf("WebDavFsDxe: No URL in load options\n");
        return EFI_INVALID_PARAMETER;
    }

    // Allocate private context
    WebDavFsPrivate *priv = axl_calloc(1, sizeof(WebDavFsPrivate));
    if (priv == NULL) { axl_free(url_utf8); return EFI_OUT_OF_RESOURCES; }

    priv->signature = WEBDAVFS_PRIVATE_SIGNATURE;
    priv->image_handle = ImageHandle;

    // Parse URL
    int ret = parse_url(url_utf8, priv->server_addr,
                        &priv->server_port, priv->base_path,
                        sizeof(priv->base_path));
    axl_free(url_utf8);
    if (ret != 0) {
        axl_printf("WebDavFsDxe: Invalid URL\n");
        axl_free(priv);
        return EFI_INVALID_PARAMETER;
    }

    // Build base URL for AxlHttpClient
    axl_snprintf(priv->base_url, sizeof(priv->base_url),
                 "http://%d.%d.%d.%d:%d",
                 priv->server_addr[0], priv->server_addr[1],
                 priv->server_addr[2], priv->server_addr[3],
                 priv->server_port);

    axl_printf("WebDavFsDxe: Connecting to %d.%d.%d.%d:%d%s\n",
               priv->server_addr[0], priv->server_addr[1],
               priv->server_addr[2], priv->server_addr[3],
               priv->server_port, priv->base_path);

    // Initialize networking
    int net_ret = network_init((size_t)-1, NULL, 10);
    if (net_ret != 0) {
        axl_printf("WebDavFsDxe: Network init failed\n");
        axl_free(priv);
        return EFI_DEVICE_ERROR;
    }

    // Create HTTP client
    priv->http_client = axl_http_client_new();
    if (priv->http_client == NULL) {
        axl_printf("WebDavFsDxe: Failed to create HTTP client\n");
        network_cleanup();
        axl_free(priv);
        return EFI_OUT_OF_RESOURCES;
    }

    // Validate server with GET /info
    char info_url[320];
    axl_snprintf(info_url, sizeof(info_url), "%s/info", priv->base_url);

    AxlHttpClientResponse *info_resp = NULL;
    int info_ret = axl_http_get(priv->http_client, info_url, &info_resp);
    if (info_ret != 0 || info_resp == NULL || info_resp->status_code != 200) {
        axl_printf("WebDavFsDxe: Server validation failed\n");
        if (info_resp != NULL) axl_http_client_response_free(info_resp);
        axl_http_client_free(priv->http_client);
        network_cleanup();
        axl_free(priv);
        return EFI_DEVICE_ERROR;
    }
    axl_http_client_response_free(info_resp);

    // Set up SimpleFileSystem protocol
    priv->simple_fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    priv->simple_fs.OpenVolume = WebDavFsOpenVolume;

    // Create vendor device path
    WebDavFsDevicePath *dev_path = axl_malloc(sizeof(WebDavFsDevicePath));
    if (dev_path == NULL) {
        axl_http_client_free(priv->http_client);
        network_cleanup();
        axl_free(priv);
        return EFI_OUT_OF_RESOURCES;
    }
    axl_memcpy(dev_path, &mDevicePathTemplate, sizeof(WebDavFsDevicePath));
    axl_memcpy(&dev_path->Vendor.Guid, &HttpFsVendorGuid, sizeof(EFI_GUID));
    priv->device_path = (EFI_DEVICE_PATH_PROTOCOL *)dev_path;

    // Install protocols on a new handle
    priv->fs_handle = NULL;
    if (axl_service_register_multiple(&priv->fs_handle,
            "simple-fs", &priv->simple_fs,
            "device-path", priv->device_path,
            NULL) != 0) {
        axl_printf("WebDavFsDxe: Protocol install failed\n");
        axl_free(dev_path);
        axl_http_client_free(priv->http_client);
        network_cleanup();
        axl_free(priv);
        return EFI_DEVICE_ERROR;
    }

    // Register unload handler
    axl_driver_set_unload(WebDavFsDriverUnload);

    // Connect controllers so Shell sees the new FS mapping
    axl_driver_connect_handle(priv->fs_handle);

    mPrivate = priv;
    axl_printf("WebDavFsDxe: Mounted successfully\n");
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsDriverUnload(
    EFI_HANDLE ImageHandle
)
{
    if (mPrivate == NULL) return EFI_SUCCESS;

    // Uninstall protocols
    axl_service_unregister(mPrivate->fs_handle, "simple-fs", &mPrivate->simple_fs);
    axl_service_unregister(mPrivate->fs_handle, "device-path", mPrivate->device_path);

    // Close HTTP client
    axl_http_client_free(mPrivate->http_client);

    // Clean up networking
    network_cleanup();

    // Free resources
    if (mPrivate->device_path != NULL) {
        axl_free(mPrivate->device_path);
    }
    axl_free(mPrivate);
    mPrivate = NULL;

    axl_printf("WebDavFsDxe: Unmounted\n");
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// OpenVolume
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsOpenVolume(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL              **Root
)
{
    WebDavFsPrivate *priv = WEBDAVFS_PRIVATE_FROM_SIMPLE_FS(This);

    WebDavFsFileCtx *root_file = webdavfs_create_file_handle(
        priv, priv->base_path, true, 0);
    if (root_file == NULL) return EFI_OUT_OF_RESOURCES;

    root_file->is_root = true;
    *Root = &root_file->file;
    return EFI_SUCCESS;
}
