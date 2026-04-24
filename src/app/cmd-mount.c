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

int
cmd_mount(int argc, char **argv)
{
    static const AxlConfigDesc mount_descs[] = {
        { "read-only", AXL_CFG_BOOL, "false", 'r', "Mount read-only", 0, 0 },
        { "help",      AXL_CFG_BOOL, "false", 'h', "Show help",       0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(mount_descs, NULL, NULL);
    if (cfg == NULL) return 1;
    axl_config_parse_args(cfg, argc, argv);

    if (axl_config_get_bool(cfg, "help") || axl_config_pos_count(cfg) < 1) {
        axl_config_usage(cfg, "axl-webfs mount", "<URL> [OPTIONS]");
        axl_config_free(cfg);
        return axl_config_pos_count(cfg) < 1 ? 1 : 0;
    }

    const char *url = axl_config_pos(cfg, 0);
    axl_config_free(cfg);

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
