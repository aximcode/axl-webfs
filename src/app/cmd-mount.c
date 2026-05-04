/** @file
  axl-webfs -- mount and umount command handlers.

  mount: Loads axl-webfs-dxe.efi via axl_driver_load, passes the server
  URL as UCS-2 load options, and starts the driver.
  umount: Unloads the previously loaded driver handle.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-internal.h"

#include <axl.h>
#include <axl/axl-driver.h>
#include <axl/axl-sys.h>

#define DRIVER_FILENAME  "axl-webfs-dxe.efi"

static const AxlGuid HttpFsVendorGuid = AXL_GUID(
    0xf47c0fa2, 0xbf67, 0x4c0d,
    0xb0, 0x5e, 0x44, 0x9a, 0x1b, 0xf3, 0x44, 0xc7);

static AxlDriverHandle mDriverHandle;

// ----------------------------------------------------------------------------
// mount command
// ----------------------------------------------------------------------------

static const AxlArgDesc kMountFlags[] = {
    { .name = "read-only", .short_name = 'r', .type = AXL_ARG_BOOL,
      .help = "Mount read-only" },
    {0}
};

static const AxlArgDesc kMountPos[] = {
    { .name = "url", .type = AXL_ARG_STRING, .required = true,
      .help = "URL of the axl-webfs server to mount" },
    {0}
};

static int
mount_handler(AxlArgs *a)
{
    const char *url = axl_args_get_string(a, "url");

    // Find the driver — search image's drivers/<arch>/, image's own
    // directory, image's drivers/, then other volumes' drivers/<arch>/.
    // axl_driver_load(DRIVER_FILENAME, ...) by itself would be CWD-
    // relative and fail when the user invoked us from a different
    // volume than where the binaries live (the Dell case).
    char drv_path[256];
    if (axl_driver_locate(DRIVER_FILENAME, drv_path, sizeof(drv_path)) != 0) {
        axl_printf("ERROR: %s not found on any mounted volume.\n",
                   DRIVER_FILENAME);
        axl_printf("       Place it next to axl-webfs.efi or under "
                   "drivers/<arch>/ on the boot disk.\n");
        return 1;
    }

    axl_printf("Loading %s...\n", drv_path);

    // Load the driver image
    AxlDriverHandle handle = NULL;
    if (axl_driver_load(drv_path, &handle) != 0) {
        axl_printf("ERROR: Cannot load %s\n", drv_path);
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
            void *devpath = NULL;
            if (axl_handle_get_service(fs_handles[i], "device-path",
                                       &devpath) == 0) {
                if (axl_device_path_has_vendor(devpath, &HttpFsVendorGuid)) {
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

int
cmd_mount(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "axl-webfs mount",
        .help         = "Mount an axl-webfs server URL as a UEFI filesystem",
        .flags        = kMountFlags,
        .positionals  = kMountPos,
        .handler      = mount_handler,
    });
}

// ----------------------------------------------------------------------------
// umount command
// ----------------------------------------------------------------------------

int
cmd_umount(int argc, char **argv)
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
        axl_printf("ERROR: No mounted axl-webfs volumes found\n");
        return 1;
    }

    bool found = false;
    for (size_t i = 0; i < fs_count; i++) {
        void *devpath = NULL;
        if (axl_handle_get_service(fs_handles[i], "device-path",
                                   &devpath) != 0)
            continue;
        if (!axl_device_path_has_vendor(devpath, &HttpFsVendorGuid))
            continue;

        /* Found the axl-webfs volume -- scan loaded images for the driver */
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
        axl_printf("ERROR: No mounted axl-webfs volumes found\n");
        return 1;
    }

    return 0;
}
