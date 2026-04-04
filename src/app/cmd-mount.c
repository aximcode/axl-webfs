/** @file
  HttpFS -- mount and umount command handlers (axl-cc port).

  mount: Loads WebDavFsDxe.efi via axl_driver_load, passes the server
  URL as UCS-2 load options, and starts the driver.
  umount: Unloads the previously loaded driver handle.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "httpfs-internal.h"

#include <axl.h>
#include <axl/axl-driver.h>
#include <axl/axl-sys.h>
#include <uefi/axl-uefi.h>

#define DRIVER_FILENAME  "WebDavFsDxe.efi"

// TODO: Verify this GUID matches the value from the .dec / build system.
// It was previously supplied via extern EFI_GUID gHttpFsVendorGuid.
static const EFI_GUID HttpFsVendorGuid = {
    0xf47c0fa2, 0xbf67, 0x4c0d,
    {0xb0, 0x5e, 0x44, 0x9a, 0x1b, 0xf3, 0x44, 0xc7}
};

static AxlDriverHandle mDriverHandle;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

/// Check if a device path contains a vendor node with the HttpFS GUID.
static bool
HasVendorNode(EFI_DEVICE_PATH_PROTOCOL *dp, const EFI_GUID *guid)
{
    if (dp == NULL) return false;

    while (!EFI_DP_IS_END(dp)) {
        if (EFI_DP_TYPE(dp) == HARDWARE_DEVICE_PATH &&
            EFI_DP_SUBTYPE(dp) == HW_VENDOR_DP) {
            VENDOR_DEVICE_PATH *v = (VENDOR_DEVICE_PATH *)dp;
            if (axl_guid_equal(&v->Guid, guid)) return true;
        }
        dp = EFI_DP_NEXT(dp);
    }
    return false;
}

// ----------------------------------------------------------------------------
// mount command
// ----------------------------------------------------------------------------

int
CmdMount(int argc, char **argv)
{
    static const AxlOpt mount_opts[] = {
        { 'r', NULL, AXL_OPT_FLAG, NULL, "Mount read-only" },
        { 'h', NULL, AXL_OPT_FLAG, NULL, "Show help" },
        { 0, NULL, 0, NULL, NULL }
    };

    AxlArgs *args = axl_args_parse(argc, argv, mount_opts);
    if (args == NULL || axl_args_pos_count(args) < 1 || axl_args_flag(args, 'h')) {
        axl_args_usage("HttpFS mount", "<URL> [OPTIONS]", mount_opts);
        axl_args_free(args);
        return (args == NULL || axl_args_pos_count(args) < 1) ? 1 : 0;
    }

    const char *url = axl_args_pos(args, 0);
    axl_args_free(args);

    axl_printf("Loading %s...\n", DRIVER_FILENAME);

    // Load the driver image
    AxlDriverHandle handle = NULL;
    if (axl_driver_load(DRIVER_FILENAME, &handle) != 0) {
        axl_printf("ERROR: Cannot load %s\n", DRIVER_FILENAME);
        return 1;
    }

    // Convert URL from UTF-8 to UCS-2 for load options
    unsigned short *url_w = axl_utf8_to_ucs2(url);
    if (url_w == NULL) {
        axl_printf("ERROR: UTF-8 to UCS-2 conversion failed\n");
        axl_driver_unload(handle);
        return 1;
    }

    size_t wlen = 0;
    while (url_w[wlen]) wlen++;
    size_t url_w_size = (wlen + 1) * sizeof(unsigned short);

    if (axl_driver_set_load_options(handle, url_w, url_w_size) != 0) {
        axl_printf("ERROR: Cannot set load options\n");
        axl_free(url_w);
        axl_driver_unload(handle);
        return 1;
    }
    axl_free(url_w);

    // Start the driver
    axl_printf("Connecting to %s...\n", url);
    if (axl_driver_start(handle) != 0) {
        axl_printf("ERROR: Driver start failed\n");
        axl_driver_unload(handle);
        return 1;
    }

    // Store handle for umount
    mDriverHandle = handle;

    // Find the new FS handle by looking for our vendor GUID in device paths
    void **fs_handles = NULL;
    size_t fs_count = 0;
    if (axl_service_enumerate("simple-fs", &fs_handles, &fs_count) == 0) {
        for (size_t i = 0; i < fs_count; i++) {
            EFI_DEVICE_PATH_PROTOCOL *devpath = NULL;
            if (axl_handle_get_service(fs_handles[i], "device-path",
                                       (void **)&devpath) == 0) {
                if (HasVendorNode(devpath, &HttpFsVendorGuid)) {
                    axl_printf("Mounted as FS handle %p\n", fs_handles[i]);
                    axl_printf("Use 'map -r' to refresh Shell mappings.\n");
                    break;
                }
            }
        }
        axl_free(fs_handles);
    }

    return 0;
}

// ----------------------------------------------------------------------------
// umount command
// ----------------------------------------------------------------------------

int
CmdUmount(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Try stored handle from this process first */
    if (mDriverHandle != NULL) {
        axl_printf("Unmounting...\n");
        if (axl_driver_unload(mDriverHandle) == 0) {
            mDriverHandle = NULL;
            axl_printf("Unmounted successfully.\n");
            return 0;
        }
        mDriverHandle = NULL;
    }

    /* Scan FS handles for our vendor GUID device path */
    void **fs_handles = NULL;
    size_t fs_count = 0;
    if (axl_service_enumerate("simple-fs", &fs_handles, &fs_count) != 0 ||
        fs_count == 0) {
        axl_free(fs_handles);
        axl_printf("ERROR: No mounted HttpFS volumes found\n");
        return 1;
    }

    bool found = false;
    for (size_t i = 0; i < fs_count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *devpath = NULL;
        if (axl_handle_get_service(fs_handles[i], "device-path",
                                   (void **)&devpath) != 0)
            continue;
        if (!HasVendorNode(devpath, &HttpFsVendorGuid))
            continue;

        /* Found the HttpFS volume — scan loaded images for the driver */
        void **img_handles = NULL;
        size_t img_count = 0;
        if (axl_service_enumerate("loaded-image", &img_handles, &img_count) != 0) {
            axl_free(img_handles);
            continue;
        }

        for (size_t j = 0; j < img_count; j++) {
            AxlDriverHandle drv = img_handles[j];
            axl_printf("Unmounting...\n");
            if (axl_driver_unload(drv) == 0) {
                found = true;
                axl_printf("Unmounted successfully.\n");
                break;
            }
        }
        axl_free(img_handles);
        if (found) break;
    }

    axl_free(fs_handles);

    if (!found) {
        axl_printf("ERROR: No mounted HttpFS volumes found\n");
        return 1;
    }

    return 0;
}
