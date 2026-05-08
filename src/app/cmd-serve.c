/** @file
  axl-webfs -- foreground `serve` verb.

  Parses CLI args into a ServeCoreOpts, brings the HTTP server up via
  serve_core_setup, prints the interactive banner, and runs the loop
  until ESC is pressed. The HTTP plumbing (route handlers, permission
  middleware, server bring-up) lives in src/serve/serve-core.c so the
  resident webfs-server-dxe driver can reuse it.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-internal.h"

#include <axl.h>
#include "net/network.h"
#include "serve/serve-core.h"
#include "transfer/file-transfer.h"


// ----------------------------------------------------------------------------
// ESC key handler for the event loop
// ----------------------------------------------------------------------------

static AxlLoop *mServeLoop = NULL;

static bool
esc_key_handler(AxlInputKey key, void *data)
{
    (void)data;

    if (key.scan_code == 0x17) {  /* SCAN_ESC */
        axl_printf("\nESC -- stopping server.\n");
        if (mServeLoop != NULL) {
            axl_loop_quit(mServeLoop);
        }

        return false;
    }

    return true;
}

// ----------------------------------------------------------------------------
// Main serve command
// ----------------------------------------------------------------------------

const AxlArgDesc webfs_serve_flags[] = {
    { .name = "port",       .short_name = 'p', .type = AXL_ARG_U16,
      .default_value = "8080", .help = "Listen port" },
    { .name = "nic",        .short_name = 'n', .type = AXL_ARG_U32,
      .help = "NIC index (default: auto)" },
    { .name = "timeout",    .short_name = 't', .type = AXL_ARG_U32,
      .help = "Idle timeout in seconds (default: 0 = none)" },
    { .name = "mode",       .short_name = 'm', .type = AXL_ARG_CHOICE,
      .choices = (const char *const []){"read-write", "read-only",
                                        "write-only", NULL},
      .default_value = "read-write",
      .help = "Permission mode: read-write (default), read-only "
              "(block PUT/POST/DELETE), or write-only (block GET)" },
    { .name = "verbose",    .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose logging" },
    { .name = "source",                            .type = AXL_ARG_STRING,
      .help = "Bind listener to interface with this station IPv4 (auto if unset)" },
    {0}
};

int
webfs_serve_handler(AxlArgs *a)
{
    const char *mode = axl_args_get_string(a, "mode");

    ServeCoreOpts opts = {
        .port             = (uint16_t)axl_args_get_uint(a, "port"),
        .nic_index        = axl_args_get_string(a, "nic") != NULL
                          ? (size_t)axl_args_get_uint(a, "nic") : (size_t)-1,
        .idle_timeout_sec = (size_t)axl_args_get_uint(a, "timeout"),
        .read_only        = axl_streql(mode, "read-only"),
        .write_only       = axl_streql(mode, "write-only"),
        .verbose          = axl_args_get_bool(a, "verbose"),
        .source           = axl_args_get_string(a, "source"),
    };

    ServeCore core;
    if (serve_core_setup(&opts, &core) != AXL_OK) {
        axl_printf("ERROR: serve bring-up failed\n");
        return 1;
    }

    /* Banner */
    axl_printf("\naxl-webfs v%s -- UEFI HTTP File Server\n", AXL_WEBFS_VERSION);
    axl_printf("Listening on %d.%d.%d.%d:%d\n",
        core.addr[0], core.addr[1], core.addr[2], core.addr[3], opts.port);
    axl_printf("Mode: %s\n", mode);

    axl_printf("Volumes:\n");
    size_t vcount = ft_get_volume_count();
    for (size_t i = 0; i < vcount; i++) {
        FtVolume vol;
        ft_get_volume(i, &vol);
        axl_printf("  %s:\n", vol.name);
    }

    axl_printf("Press ESC to stop.\n\n");

    /* Foreground-specific: ESC handler, then block in axl_loop_run. */
    mServeLoop = core.loop;
    axl_loop_add_key_press(core.loop, esc_key_handler, NULL);

    int status = axl_loop_run(core.loop);

    mServeLoop = NULL;
    serve_core_teardown(&core);
    return status;
}
