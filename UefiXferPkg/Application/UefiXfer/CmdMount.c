/** @file
  UefiXfer — mount and umount command handlers.

  mount: Locates WebDavFsDxe.efi in the same directory as UefiXfer.efi,
  loads it via LoadImage/StartImage with the server URL in load options.
  umount: Finds the driver by vendor GUID device path and unloads it.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiXferInternal.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DevicePathLib.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>

extern EFI_GUID gUefiXferVendorGuid;

#define DRIVER_FILENAME  L"WebDavFsDxe.efi"

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

/// Build a device path to WebDavFsDxe.efi in the same directory as UefiXfer.efi.
static EFI_DEVICE_PATH_PROTOCOL * BuildDriverDevicePath(
    IN EFI_HANDLE  ImageHandle
) {
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    EFI_STATUS Status = gBS->OpenProtocol(
        ImageHandle, &gEfiLoadedImageProtocolGuid,
        (VOID **)&LoadedImage, ImageHandle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status) || LoadedImage == NULL) return NULL;

    // Get the file path portion of our device path
    EFI_DEVICE_PATH_PROTOCOL *FilePath = LoadedImage->FilePath;
    if (FilePath == NULL) return NULL;

    // Walk the device path to find the file path node and extract directory
    // The file path node contains something like "\UefiXfer.efi"
    // We need to replace the filename with DRIVER_FILENAME

    // Build a new file path for the driver in the same directory
    // Use FileDevicePath helper with the device handle
    return FileDevicePath(LoadedImage->DeviceHandle, DRIVER_FILENAME);
}

/// Check if a device path contains a vendor node with our GUID.
static BOOLEAN HasXferVendorNode(IN EFI_DEVICE_PATH_PROTOCOL *DevPath) {
    if (DevPath == NULL) return FALSE;

    while (!IsDevicePathEnd(DevPath)) {
        if (DevicePathType(DevPath) == HARDWARE_DEVICE_PATH &&
            DevicePathSubType(DevPath) == HW_VENDOR_DP) {
            VENDOR_DEVICE_PATH *Vendor = (VENDOR_DEVICE_PATH *)DevPath;
            if (CompareGuid(&Vendor->Guid, &gUefiXferVendorGuid)) {
                return TRUE;
            }
        }
        DevPath = NextDevicePathNode(DevPath);
    }
    return FALSE;
}

// ----------------------------------------------------------------------------
// mount command
// ----------------------------------------------------------------------------

EFI_STATUS CmdMount(
    IN EFI_HANDLE  ImageHandle,
    IN UINTN       Argc,
    IN CHAR16      **Argv
) {
    if (Argc < 1) {
        Print(L"Usage: UefiXfer mount <url> [-r]\n");
        Print(L"  url: http://<ip>:<port>/[path]\n");
        Print(L"  -r:  mount read-only\n");
        return EFI_INVALID_PARAMETER;
    }

    CHAR16 *Url = Argv[0];

    // Build device path to the driver
    EFI_DEVICE_PATH_PROTOCOL *DriverPath = BuildDriverDevicePath(ImageHandle);
    if (DriverPath == NULL) {
        Print(L"ERROR: Cannot locate %s\n", DRIVER_FILENAME);
        return EFI_NOT_FOUND;
    }

    Print(L"Loading %s...\n", DRIVER_FILENAME);

    // Load the driver image
    EFI_HANDLE DriverHandle = NULL;
    EFI_STATUS Status = gBS->LoadImage(
        FALSE, ImageHandle, DriverPath, NULL, 0, &DriverHandle);
    FreePool(DriverPath);

    if (EFI_ERROR(Status)) {
        Print(L"ERROR: LoadImage failed: %r\n", Status);
        return Status;
    }

    // Set load options to the URL string
    EFI_LOADED_IMAGE_PROTOCOL *DriverImage = NULL;
    Status = gBS->OpenProtocol(
        DriverHandle, &gEfiLoadedImageProtocolGuid,
        (VOID **)&DriverImage, ImageHandle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) {
        gBS->UnloadImage(DriverHandle);
        Print(L"ERROR: Cannot access driver image: %r\n", Status);
        return Status;
    }

    // URL as load options (CHAR16 string including null terminator)
    DriverImage->LoadOptions = Url;
    DriverImage->LoadOptionsSize = (UINT32)((StrLen(Url) + 1) * sizeof(CHAR16));

    // Start the driver
    Print(L"Connecting to %s...\n", Url);
    Status = gBS->StartImage(DriverHandle, NULL, NULL);
    if (EFI_ERROR(Status)) {
        gBS->UnloadImage(DriverHandle);
        Print(L"ERROR: Driver start failed: %r\n", Status);
        return Status;
    }

    // Find the new FS handle by looking for our vendor GUID in device paths
    EFI_HANDLE *FsHandles = NULL;
    UINTN FsCount = 0;
    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
        NULL, &FsCount, &FsHandles);

    if (!EFI_ERROR(Status)) {
        for (UINTN i = 0; i < FsCount; i++) {
            EFI_DEVICE_PATH_PROTOCOL *DevPath = NULL;
            Status = gBS->OpenProtocol(
                FsHandles[i], &gEfiDevicePathProtocolGuid,
                (VOID **)&DevPath, ImageHandle, NULL,
                EFI_OPEN_PROTOCOL_GET_PROTOCOL);
            if (!EFI_ERROR(Status) && HasXferVendorNode(DevPath)) {
                Print(L"Mounted as FS handle %p\n", FsHandles[i]);
                Print(L"Use 'map -r' to refresh Shell mappings.\n");
                break;
            }
        }
        FreePool(FsHandles);
    }

    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// umount command
// ----------------------------------------------------------------------------

EFI_STATUS CmdUmount(
    IN EFI_HANDLE  ImageHandle,
    IN UINTN       Argc,
    IN CHAR16      **Argv
) {
    // Find the WebDavFsDxe driver handle by looking for loaded images
    // with our vendor GUID device path on their installed FS handle.

    EFI_HANDLE *FsHandles = NULL;
    UINTN FsCount = 0;
    EFI_STATUS Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
        NULL, &FsCount, &FsHandles);

    if (EFI_ERROR(Status) || FsCount == 0) {
        Print(L"ERROR: No mounted XferMount volumes found\n");
        return EFI_NOT_FOUND;
    }

    BOOLEAN Found = FALSE;
    for (UINTN i = 0; i < FsCount; i++) {
        EFI_DEVICE_PATH_PROTOCOL *DevPath = NULL;
        Status = gBS->OpenProtocol(
            FsHandles[i], &gEfiDevicePathProtocolGuid,
            (VOID **)&DevPath, ImageHandle, NULL,
            EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(Status) || !HasXferVendorNode(DevPath)) continue;

        // Find the loaded image that owns this — iterate all images
        EFI_HANDLE *ImageHandles = NULL;
        UINTN ImageCount = 0;
        Status = gBS->LocateHandleBuffer(
            ByProtocol, &gEfiLoadedImageProtocolGuid,
            NULL, &ImageCount, &ImageHandles);

        if (!EFI_ERROR(Status)) {
            for (UINTN j = 0; j < ImageCount; j++) {
                EFI_LOADED_IMAGE_PROTOCOL *LI = NULL;
                gBS->OpenProtocol(
                    ImageHandles[j], &gEfiLoadedImageProtocolGuid,
                    (VOID **)&LI, ImageHandle, NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL);
                if (LI == NULL) continue;

                // Check if this image has an Unload function (our driver does)
                if (LI->Unload != NULL) {
                    // Try to unload — if it's our driver, it will work
                    Print(L"Unmounting...\n");
                    Status = gBS->UnloadImage(ImageHandles[j]);
                    if (!EFI_ERROR(Status)) {
                        Print(L"Unmounted successfully.\n");
                        Found = TRUE;
                        break;
                    }
                }
            }
            FreePool(ImageHandles);
        }

        if (Found) break;
    }

    FreePool(FsHandles);

    if (!Found) {
        Print(L"ERROR: No mounted XferMount volumes found\n");
        return EFI_NOT_FOUND;
    }

    return EFI_SUCCESS;
}
