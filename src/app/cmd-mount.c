/** @file
  axl-webfs -- mount and umount command handlers.

  mount: populates g_mount_opts.url (and read_only) from AxlArgs,
  then hands an AxlServiceDeploy to axl_service_start_embedded.
  The SDK loads the embedded webfs-mount-dxe.efi image, serializes
  g_mount_opts via mount_descs into LoadOptions, and calls the
  driver's AXL_SERVICE_DRIVER trampoline, which decodes and runs
  mount_setup. setup() installs EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on
  a fresh handle (vendor device path), so the Shell sees a new FSn:.
  Mount returns immediately -- no supervise loop, since the driver
  serves protocol calls synchronously from outside the loop.

  umount: calls axl_service_stop, which resolves the driver image's
  handle by the service's name-derived GUID and unloads it. The
  driver's unload stub runs mount_teardown to uninstall the protocols
  and free runtime state.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include <axl.h>
#include <axl/axl-embed.h>
#include <axl/axl-service.h>

#include "mount/webfs-mount.h"

/* Embedded webfs-mount-dxe.efi blob -- spliced in by
   `axl-cc --embed ...=axl_webfs_mount_dxe` (see Makefile). */
AXL_EMBED_DECLARE(axl_webfs_mount_dxe);

static AxlServiceDeploy
mount_make_deploy(void)
{
    AxlServiceDeploy d = {
        .service         = &webfs_mount,
        .driver_blob     = AXL_EMBED_DATA(axl_webfs_mount_dxe),
        .driver_name     = "axl-webfs-mount-dxe.efi",
    };
    d.driver_blob_len = AXL_EMBED_SIZE(axl_webfs_mount_dxe);
    return d;
}

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
    g_mount_opts.url       = axl_args_get_string(a, "url");
    g_mount_opts.read_only = axl_args_get_bool(a, "read-only");

    AxlServiceDeploy deploy = mount_make_deploy();

    if (axl_service_is_running(&deploy)) {
        axl_printf("axl-webfs: already mounted (run umount first)\n");
        return 0;
    }

    int rc = axl_service_start_embedded(&deploy);
    if (rc != AXL_OK) {
        axl_printf("ERROR: mount failed (rc=%d)\n", rc);
        return 1;
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

    AxlServiceDeploy deploy = mount_make_deploy();

    if (!axl_service_is_running(&deploy)) {
        axl_printf("axl-webfs: nothing to unmount\n");
        return 0;
    }

    int rc = axl_service_stop(&deploy);
    if (rc != AXL_OK) {
        axl_printf("ERROR: umount failed (rc=%d)\n", rc);
        return 1;
    }
    return 0;
}
