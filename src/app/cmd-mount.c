/** @file
  axl-webfs -- mount and umount command handlers.

  mount: LoadImages the embedded axl-webfs-dxe.efi blob via
  axl_driver_load_buffer, installs the server URL as UCS-2 LoadOptions,
  and starts the driver. The driver image is .incbin'd into
  axl-webfs.efi by axl-cc --embed (see Makefile), so mount works as
  a single binary -- no sidecar driver file on disk.

  umount: Unloads the previously loaded driver handle. Falls back to
  scanning loaded images for the axl-webfs vendor GUID device path
  when invoked from a different process than the one that mounted.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include <axl.h>
#include <axl/axl-driver.h>
#include <axl/axl-embed.h>
#include <axl/axl-sys.h>

/* Embedded axl-webfs-dxe.efi blob -- spliced in by
   `axl-cc --embed ...=axl_webfs_mount_dxe` (see Makefile). */
AXL_EMBED_DECLARE(axl_webfs_mount_dxe);

static const AxlGuid HttpFsVendorGuid = AXL_GUID(
    0xf47c0fa2, 0xbf67, 0x4c0d,
    0xb0, 0x5e, 0x44, 0x9a, 0x1b, 0xf3, 0x44, 0xc7);

static AxlDriverHandle mDriverHandle;

// ----------------------------------------------------------------------------
// mount command
// ----------------------------------------------------------------------------

const AxlArgDesc webfs_mount_flags[] = {
    { .name = "read-only", .short_name = 'r', .type = AXL_ARG_BOOL,
      .help = "Mount read-only" },
    {0}
};

const AxlArgDesc webfs_mount_pos[] = {
    { .name = "url", .type = AXL_ARG_STRING, .required = true,
      .help = "URL of the axl-webfs server to mount" },
    {0}
};

int
webfs_mount_handler(AxlArgs *a)
{
    const char *url = axl_args_get_string(a, "url");

    /* Load the embedded mount driver image. axl_driver_load_buffer
       wraps gBS->LoadImage with SourceBuffer/SourceSize and returns
       a handle for set_load_options + start + unload. We can't use
       axl_driver_ensure_with_embedded here -- its LocateProtocol
       short-circuit on EFI_FILE_PROTOCOL would match any unrelated
       FS handle and skip the load entirely, and it doesn't expose
       the AxlDriverHandle the umount path needs. */
    AxlDriverHandle handle = NULL;
    if (axl_driver_load_buffer(AXL_EMBED_DATA(axl_webfs_mount_dxe),
                               AXL_EMBED_SIZE(axl_webfs_mount_dxe),
                               &handle) != 0) {
        axl_printf("ERROR: Cannot load embedded mount driver\n");
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
    if (axl_protocol_enumerate("simple-fs", &fs_handles, &fs_count) == 0) {
        for (size_t i = 0; i < fs_count; i++) {
            void *devpath = NULL;
            if (axl_handle_get_protocol(fs_handles[i], "device-path",
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
webfs_umount_handler(AxlArgs *a)
{
    (void)a;

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
    if (axl_protocol_enumerate("simple-fs", &fs_handles, &fs_count) != 0 ||
        fs_count == 0) {
        axl_free(fs_handles);
        axl_printf("ERROR: No mounted axl-webfs volumes found\n");
        return 1;
    }

    bool found = false;
    for (size_t i = 0; i < fs_count; i++) {
        void *devpath = NULL;
        if (axl_handle_get_protocol(fs_handles[i], "device-path",
                                   &devpath) != 0)
            continue;
        if (!axl_device_path_has_vendor(devpath, &HttpFsVendorGuid))
            continue;

        /* Found the axl-webfs volume -- scan loaded images for the driver */
        void **img_handles = NULL;
        size_t img_count = 0;
        if (axl_protocol_enumerate("loaded-image", &img_handles, &img_count) != 0) {
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
