/** @file
  WebDavFsDxe — Driver entry point, URL parsing, protocol install.

  Loads as a UEFI driver, parses the server URL from load options,
  establishes an HTTP connection, and installs EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
  on a new device handle so the UEFI Shell sees it as FSn:.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "WebDavFsInternal.h"

/// Module-global pointer to the driver private context (for unload path).
static WEBDAVFS_PRIVATE *mPrivate = NULL;

// ----------------------------------------------------------------------------
// URL parsing
// ----------------------------------------------------------------------------

/// Parse "http://1.2.3.4:8080/path" into components.
static EFI_STATUS ParseUrl(
    IN  CONST CHAR16      *Url,
    IN  UINTN             UrlLen,
    OUT EFI_IPv4_ADDRESS  *ServerAddr,
    OUT UINT16            *Port,
    OUT CHAR8             *BasePath,
    IN  UINTN             BasePathSize
) {
    // Convert to ASCII for easier parsing
    CHAR8 Buf[512];
    UINTN Len = UrlLen;
    if (Len >= sizeof(Buf)) Len = sizeof(Buf) - 1;
    for (UINTN i = 0; i < Len; i++) {
        Buf[i] = (CHAR8)Url[i];
    }
    Buf[Len] = '\0';

    // Skip "http://"
    CHAR8 *P = Buf;
    if (AsciiStrnCmp(P, "http://", 7) == 0) {
        P += 7;
    }

    // Parse IPv4 address
    UINT32 Octets[4] = {0};
    for (UINTN i = 0; i < 4; i++) {
        while (*P >= '0' && *P <= '9') {
            Octets[i] = Octets[i] * 10 + (*P - '0');
            P++;
        }
        if (Octets[i] > 255) return EFI_INVALID_PARAMETER;
        if (i < 3) {
            if (*P != '.') return EFI_INVALID_PARAMETER;
            P++;
        }
    }

    ServerAddr->Addr[0] = (UINT8)Octets[0];
    ServerAddr->Addr[1] = (UINT8)Octets[1];
    ServerAddr->Addr[2] = (UINT8)Octets[2];
    ServerAddr->Addr[3] = (UINT8)Octets[3];

    // Parse optional :port
    *Port = DEFAULT_SERVER_PORT;
    if (*P == ':') {
        P++;
        UINT32 PortVal = 0;
        while (*P >= '0' && *P <= '9') {
            PortVal = PortVal * 10 + (*P - '0');
            P++;
        }
        if (PortVal > 0 && PortVal <= 65535) {
            *Port = (UINT16)PortVal;
        }
    }

    // Copy remaining path (or "/" if empty)
    if (*P == '/' && *(P + 1) != '\0') {
        AsciiStrCpyS(BasePath, BasePathSize, P);
    } else {
        AsciiStrCpyS(BasePath, BasePathSize, "/");
    }

    // Ensure trailing slash
    UINTN PathLen = AsciiStrLen(BasePath);
    if (PathLen > 0 && BasePath[PathLen - 1] != '/' && PathLen < BasePathSize - 1) {
        BasePath[PathLen] = '/';
        BasePath[PathLen + 1] = '\0';
    }

    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// Vendor device path for the mounted volume
// ----------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    VENDOR_DEVICE_PATH  Vendor;
    EFI_DEVICE_PATH_PROTOCOL End;
} WEBDAVFS_DEVICE_PATH;
#pragma pack()

static WEBDAVFS_DEVICE_PATH mDevicePathTemplate = {
    {
        { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
          { sizeof(VENDOR_DEVICE_PATH), 0 } },
        { 0 }  // GUID filled at runtime
    },
    { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
      { sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

// ----------------------------------------------------------------------------
// Driver entry and unload
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsDriverEntry (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable
    )
{
    EFI_STATUS Status;

    // Get load options (URL string)
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    Status = gBS->OpenProtocol(
        ImageHandle, &gEfiLoadedImageProtocolGuid,
        (VOID **)&LoadedImage, ImageHandle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status) || LoadedImage == NULL) {
        Print(L"WebDavFsDxe: Failed to get loaded image protocol\n");
        return EFI_DEVICE_ERROR;
    }

    if (LoadedImage->LoadOptions == NULL || LoadedImage->LoadOptionsSize < 4) {
        Print(L"WebDavFsDxe: No URL in load options\n");
        return EFI_INVALID_PARAMETER;
    }

    CHAR16 *UrlStr = (CHAR16 *)LoadedImage->LoadOptions;
    UINTN UrlLen = LoadedImage->LoadOptionsSize / sizeof(CHAR16);

    // Allocate private context
    WEBDAVFS_PRIVATE *Private = AllocateZeroPool(sizeof(WEBDAVFS_PRIVATE));
    if (Private == NULL) return EFI_OUT_OF_RESOURCES;

    Private->Signature = WEBDAVFS_PRIVATE_SIGNATURE;
    Private->ImageHandle = ImageHandle;

    // Parse URL
    Status = ParseUrl(UrlStr, UrlLen, &Private->ServerAddr,
                      &Private->ServerPort, Private->BasePath,
                      sizeof(Private->BasePath));
    if (EFI_ERROR(Status)) {
        Print(L"WebDavFsDxe: Invalid URL\n");
        FreePool(Private);
        return Status;
    }

    Print(L"WebDavFsDxe: Connecting to %d.%d.%d.%d:%d%a\n",
          Private->ServerAddr.Addr[0], Private->ServerAddr.Addr[1],
          Private->ServerAddr.Addr[2], Private->ServerAddr.Addr[3],
          Private->ServerPort, Private->BasePath);

    // Initialize networking
    Status = NetworkInit(ImageHandle, (UINTN)-1, NULL, 10);
    if (EFI_ERROR(Status)) {
        Print(L"WebDavFsDxe: Network init failed: %r\n", Status);
        FreePool(Private);
        return Status;
    }

    // Get TCP ServiceBinding handle
    Status = NetworkGetTcpServiceBinding(&Private->TcpSbHandle);
    if (EFI_ERROR(Status)) {
        Print(L"WebDavFsDxe: No TCP4 ServiceBinding: %r\n", Status);
        NetworkCleanup();
        FreePool(Private);
        return Status;
    }

    // Connect to server
    Status = HttpClientConnect(
        ImageHandle, Private->TcpSbHandle,
        Private->ServerAddr, Private->ServerPort,
        HTTP_CONNECT_TIMEOUT_MS, &Private->HttpClient);
    if (EFI_ERROR(Status)) {
        Print(L"WebDavFsDxe: HTTP connect failed: %r\n", Status);
        NetworkCleanup();
        FreePool(Private);
        return Status;
    }

    // Validate server with GET /info
    HTTP_RESPONSE_CTX InfoResp;
    Status = HttpClientRequest(
        &Private->HttpClient, "GET", "/info", NULL, 0, NULL, 0, &InfoResp);
    if (EFI_ERROR(Status) || InfoResp.StatusCode != 200) {
        Print(L"WebDavFsDxe: Server validation failed\n");
        HttpClientClose(&Private->HttpClient);
        NetworkCleanup();
        FreePool(Private);
        return EFI_DEVICE_ERROR;
    }

    // Drain info response body
    CHAR8 Drain[256];
    UINTN DrainGot = 0;
    while (!EFI_ERROR(HttpClientReadBody(
               &Private->HttpClient, &InfoResp, Drain,
               sizeof(Drain), &DrainGot)) && DrainGot > 0) {
    }

    // Set up SimpleFileSystem protocol
    Private->SimpleFs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    Private->SimpleFs.OpenVolume = WebDavFsOpenVolume;

    // Create vendor device path
    WEBDAVFS_DEVICE_PATH *DevPath = AllocateCopyPool(
        sizeof(WEBDAVFS_DEVICE_PATH), &mDevicePathTemplate);
    if (DevPath == NULL) {
        HttpClientClose(&Private->HttpClient);
        NetworkCleanup();
        FreePool(Private);
        return EFI_OUT_OF_RESOURCES;
    }
    CopyMem(&DevPath->Vendor.Guid, &gHttpFsVendorGuid, sizeof(EFI_GUID));
    Private->DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)DevPath;

    // Install protocols on a new handle
    Private->FsHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces(
        &Private->FsHandle,
        &gEfiSimpleFileSystemProtocolGuid, &Private->SimpleFs,
        &gEfiDevicePathProtocolGuid, Private->DevicePath,
        NULL);
    if (EFI_ERROR(Status)) {
        Print(L"WebDavFsDxe: Protocol install failed: %r\n", Status);
        FreePool(DevPath);
        HttpClientClose(&Private->HttpClient);
        NetworkCleanup();
        FreePool(Private);
        return Status;
    }

    // Register unload handler
    LoadedImage->Unload = WebDavFsDriverUnload;

    mPrivate = Private;
    Print(L"WebDavFsDxe: Mounted successfully\n");
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WebDavFsDriverUnload (
    IN EFI_HANDLE  ImageHandle
    )
{
    if (mPrivate == NULL) return EFI_SUCCESS;

    // Uninstall protocols
    gBS->UninstallMultipleProtocolInterfaces(
        mPrivate->FsHandle,
        &gEfiSimpleFileSystemProtocolGuid, &mPrivate->SimpleFs,
        &gEfiDevicePathProtocolGuid, mPrivate->DevicePath,
        NULL);

    // Close HTTP connection
    HttpClientClose(&mPrivate->HttpClient);

    // Clean up networking
    NetworkCleanup();

    // Free resources
    if (mPrivate->DevicePath != NULL) {
        FreePool(mPrivate->DevicePath);
    }
    FreePool(mPrivate);
    mPrivate = NULL;

    Print(L"WebDavFsDxe: Unmounted\n");
    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// OpenVolume
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
WebDavFsOpenVolume (
    IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
    OUT EFI_FILE_PROTOCOL                **Root
    )
{
    WEBDAVFS_PRIVATE *Private = WEBDAVFS_PRIVATE_FROM_SIMPLE_FS(This);

    WEBDAVFS_FILE *RootFile = WebDavFsCreateFileHandle(
        Private, Private->BasePath, TRUE, 0);
    if (RootFile == NULL) return EFI_OUT_OF_RESOURCES;

    RootFile->IsRoot = TRUE;
    *Root = &RootFile->File;
    return EFI_SUCCESS;
}
