/** @file
  axl-webfs -- mount and umount command handlers.

  mount: populates g_mount_opts.url (and read_only) from AxlArgs,
  then hands an AxlServiceDeploy to axl_service_start_embedded.
  The SDK loads the embedded webfs-mount-dxe.efi image, serializes
  g_mount_opts via mount_descs into LoadOptions, and calls the
  driver's AXL_SERVICE_DRIVER trampoline, which decodes and runs
  mount_setup. setup() publishes an AxlFsProvider via
  axl_fs_provider_publish; the SDK synthesizes
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL + EFI_FILE_PROTOCOL on top and
  installs them on a fresh vendor-path handle, so the Shell sees
  a new FSn:.
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
    /* embedded_only: load the baked-in blob directly and skip the SDK's
       4-path disk search. Without it, search candidate #1 is
       <image_dir>/<driver_name> -- exactly where devkit staged a loose
       axl-webfs-mount-dxe.efi before 1aa5133 -- so a stale copy on an
       older ESP would silently shadow the driver embedded in this
       launcher. driver_name is kept only to name the loaded image in the
       SDK's log line; driver_path must stay unset (mutually exclusive). */
    AxlServiceDeploy d = {
        .service         = &webfs_mount,
        .driver_blob     = AXL_EMBED_DATA(axl_webfs_mount_dxe),
        .driver_name     = "axl-webfs-mount-dxe.efi",
        .embedded_only   = true,
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
    { .name = "protocol", .short_name = 'p', .type = AXL_ARG_STRING,
      .help = "Wire protocol: auto (default), json, or dav" },
    { .name = "auth", .short_name = 'a', .type = AXL_ARG_STRING,
      .help = "HTTP auth: basic:user:token | bearer:token" },
    { .name = "nic", .short_name = 'n', .type = AXL_ARG_U64,
      .help = "NIC index to bring up (default: auto-detect)" },
    { .name = "source-ip", .short_name = 's', .type = AXL_ARG_STRING,
      .help = "IPv4 to bind the outbound socket to "
              "(e.g. 192.168.1.50). Empty = stack picks." },
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
    g_mount_opts.protocol  = axl_args_get_string(a, "protocol");
    if (g_mount_opts.protocol == NULL || g_mount_opts.protocol[0] == '\0')
        g_mount_opts.protocol = "auto";
    g_mount_opts.auth      = axl_args_get_string(a, "auth");
    if (g_mount_opts.auth == NULL)
        g_mount_opts.auth = "";
    /* AxlArgs probe: get_string returns NULL when the flag wasn't
       passed (regardless of the typed accessor), so we use it to
       distinguish "user gave --nic 0" from "no --nic at all". The
       AXL_NET_NIC_AUTO sentinel lands in mount_descs's default
       value too so the driver-side AxlConfig auto-apply produces
       the same result. */
    g_mount_opts.net.nic_index = (axl_args_get_string(a, "nic") != NULL)
                                     ? axl_args_get_uint(a, "nic")
                                     : AXL_NET_NIC_AUTO;
    g_mount_opts.net.local_ip  = axl_args_get_string(a, "source-ip");
    if (g_mount_opts.net.local_ip == NULL)
        g_mount_opts.net.local_ip = "";

    AxlServiceDeploy deploy = mount_make_deploy();

    if (axl_service_is_running(&deploy)) {
        axl_printf("axl-webfs: already mounted (run umount first)\n");
        return 0;
    }

    /* AxlStatus since SDK v3.0.0 -- AXL_NOT_FOUND now separates "no
       candidate produced a registered protocol" (the driver image
       itself) from AXL_ERR's descriptor / LoadOptions-overflow cases,
       which are the operator's own inputs. Worth telling apart: they
       point at completely different things to go look at. */
    AxlStatus rc = axl_service_start_embedded(&deploy);
    if (rc != AXL_OK) {
        if (rc == AXL_NOT_FOUND) {
            axl_printf("ERROR: mount: the mount driver image could not "
                       "be loaded or failed to start\n");
        } else {
            axl_printf("ERROR: mount failed (rc=%d)\n", (int)rc);
        }
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
