/** @file
  WebDavFsDxe -- Driver entry point, URL parsing, protocol install (axl-cc port).

  Loads as a UEFI driver, parses the server URL from load options,
  establishes an HTTP connection, and installs EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
  on a new device handle so the UEFI Shell sees it as FSn:.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "WebDavFsInternal.h"

/// Vendor GUID for the WebDavFs device path node.
static const EFI_GUID HttpFsVendorGuid = {
    0xf47c0fa2, 0xbf67, 0x4c0d,
    {0xb0, 0x5e, 0x44, 0x9a, 0x1b, 0xf3, 0x44, 0xc7}
};

/// Module-global pointer to the driver private context (for unload path).
static WEBDAVFS_PRIVATE *mPrivate = NULL;

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

/// Parse "http://1.2.3.4:8080/path" into components using axl_url_parse.
static int
ParseUrl(
    const char       *UrlUtf8,
    EFI_IPv4_ADDRESS *ServerAddr,
    UINT16           *Port,
    char             *BasePath,
    size_t            BasePathSize
)
{
    AxlUrl *Parsed = NULL;
    if (axl_url_parse(UrlUtf8, &Parsed) != 0)
        return -1;

    // Parse IPv4 address from host string
    unsigned int Octets[4] = {0};
    const char *P = Parsed->host;
    for (int i = 0; i < 4; i++) {
        unsigned int Val = 0;
        while (*P >= '0' && *P <= '9') {
            Val = Val * 10 + (unsigned int)(*P - '0');
            P++;
        }
        if (Val > 255) { axl_url_free(Parsed); return -1; }
        Octets[i] = Val;
        if (i < 3) {
            if (*P != '.') { axl_url_free(Parsed); return -1; }
            P++;
        }
    }

    ServerAddr->Addr[0] = (UINT8)Octets[0];
    ServerAddr->Addr[1] = (UINT8)Octets[1];
    ServerAddr->Addr[2] = (UINT8)Octets[2];
    ServerAddr->Addr[3] = (UINT8)Octets[3];

    *Port = (Parsed->port != 0) ? Parsed->port : DEFAULT_SERVER_PORT;

    const char *UrlPath = (Parsed->path != NULL && Parsed->path[0] != '\0')
                          ? Parsed->path : "/";
    axl_strlcpy(BasePath, UrlPath, BasePathSize);

    size_t PathLen = axl_strlen(BasePath);
    if (PathLen > 0 && BasePath[PathLen - 1] != '/' && PathLen < BasePathSize - 1) {
        BasePath[PathLen] = '/';
        BasePath[PathLen + 1] = '\0';
    }

    axl_url_free(Parsed);
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
    EFI_STATUS Status;

    // Initialize AXL runtime (sets gST/gBS/gRT, enables axl_printf)
    axl_driver_init(ImageHandle, SystemTable);

    // Get URL from load options (UTF-8, allocated by SDK)
    char *UrlUtf8 = axl_driver_get_load_options();
    if (UrlUtf8 == NULL) {
        axl_printf("WebDavFsDxe: No URL in load options\n");
        return EFI_INVALID_PARAMETER;
    }

    // Allocate private context
    WEBDAVFS_PRIVATE *Private = axl_calloc(1, sizeof(WEBDAVFS_PRIVATE));
    if (Private == NULL) { axl_free(UrlUtf8); return EFI_OUT_OF_RESOURCES; }

    Private->Signature = WEBDAVFS_PRIVATE_SIGNATURE;
    Private->ImageHandle = ImageHandle;

    // Parse URL
    int Ret = ParseUrl(UrlUtf8, &Private->ServerAddr,
                       &Private->ServerPort, Private->BasePath,
                       sizeof(Private->BasePath));
    axl_free(UrlUtf8);
    if (Ret != 0) {
        axl_printf("WebDavFsDxe: Invalid URL\n");
        axl_free(Private);
        return EFI_INVALID_PARAMETER;
    }

    // Build base URL for AxlHttpClient
    axl_snprintf(Private->BaseUrl, sizeof(Private->BaseUrl),
                 "http://%d.%d.%d.%d:%d",
                 Private->ServerAddr.Addr[0], Private->ServerAddr.Addr[1],
                 Private->ServerAddr.Addr[2], Private->ServerAddr.Addr[3],
                 Private->ServerPort);

    axl_printf("WebDavFsDxe: Connecting to %d.%d.%d.%d:%d%s\n",
               Private->ServerAddr.Addr[0], Private->ServerAddr.Addr[1],
               Private->ServerAddr.Addr[2], Private->ServerAddr.Addr[3],
               Private->ServerPort, Private->BasePath);

    // Initialize networking
    int NetRet = network_init((size_t)-1, NULL, 10);
    if (NetRet != 0) {
        axl_printf("WebDavFsDxe: Network init failed\n");
        axl_free(Private);
        return EFI_DEVICE_ERROR;
    }

    // Create HTTP client
    Private->HttpClient = axl_http_client_new();
    if (Private->HttpClient == NULL) {
        axl_printf("WebDavFsDxe: Failed to create HTTP client\n");
        network_cleanup();
        axl_free(Private);
        return EFI_OUT_OF_RESOURCES;
    }

    // Validate server with GET /info
    char InfoUrl[320];
    axl_snprintf(InfoUrl, sizeof(InfoUrl), "%s/info", Private->BaseUrl);

    AxlHttpClientResponse *InfoResp = NULL;
    int InfoRet = axl_http_get(Private->HttpClient, InfoUrl, &InfoResp);
    if (InfoRet != 0 || InfoResp == NULL || InfoResp->status_code != 200) {
        axl_printf("WebDavFsDxe: Server validation failed\n");
        if (InfoResp != NULL) axl_http_client_response_free(InfoResp);
        axl_http_client_free(Private->HttpClient);
        network_cleanup();
        axl_free(Private);
        return EFI_DEVICE_ERROR;
    }
    axl_http_client_response_free(InfoResp);

    // Set up SimpleFileSystem protocol
    Private->SimpleFs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    Private->SimpleFs.OpenVolume = WebDavFsOpenVolume;

    // Create vendor device path
    WebDavFsDevicePath *DevPath = axl_malloc(sizeof(WebDavFsDevicePath));
    if (DevPath == NULL) {
        axl_http_client_free(Private->HttpClient);
        network_cleanup();
        axl_free(Private);
        return EFI_OUT_OF_RESOURCES;
    }
    axl_memcpy(DevPath, &mDevicePathTemplate, sizeof(WebDavFsDevicePath));
    axl_memcpy(&DevPath->Vendor.Guid, &HttpFsVendorGuid, sizeof(EFI_GUID));
    Private->DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)DevPath;

    // Install protocols on a new handle
    Private->FsHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces(
        &Private->FsHandle,
        &gEfiSimpleFileSystemProtocolGuid, &Private->SimpleFs,
        &gEfiDevicePathProtocolGuid, Private->DevicePath,
        NULL);
    if (EFI_ERROR(Status)) {
        axl_printf("WebDavFsDxe: Protocol install failed: 0x%lx\n",
                   (unsigned long)Status);
        axl_free(DevPath);
        axl_http_client_free(Private->HttpClient);
        network_cleanup();
        axl_free(Private);
        return Status;
    }

    // Register unload handler via loaded image protocol
    {
        EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
        gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid,
                            (VOID **)&LoadedImage);
        if (LoadedImage != NULL)
            LoadedImage->Unload = WebDavFsDriverUnload;
    }

    // Connect controllers so Shell sees the new FS mapping
    axl_driver_connect_handle(Private->FsHandle);

    mPrivate = Private;
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
    gBS->UninstallMultipleProtocolInterfaces(
        mPrivate->FsHandle,
        &gEfiSimpleFileSystemProtocolGuid, &mPrivate->SimpleFs,
        &gEfiDevicePathProtocolGuid, mPrivate->DevicePath,
        NULL);

    // Close HTTP client
    axl_http_client_free(mPrivate->HttpClient);

    // Clean up networking
    network_cleanup();

    // Free resources
    if (mPrivate->DevicePath != NULL) {
        axl_free(mPrivate->DevicePath);
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
    WEBDAVFS_PRIVATE *Private = WEBDAVFS_PRIVATE_FROM_SIMPLE_FS(This);

    WEBDAVFS_FILE *RootFile = WebDavFsCreateFileHandle(
        Private, Private->BasePath, true, 0);
    if (RootFile == NULL) return EFI_OUT_OF_RESOURCES;

    RootFile->IsRoot = true;
    *Root = &RootFile->File;
    return EFI_SUCCESS;
}
