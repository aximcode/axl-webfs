/** @file
  axl-webfs -- foreground `serve` verb.

  Populates the shared g_serve_opts from CLI arguments. With --detach,
  hands an AxlServiceDeploy to axl_service_launch_embedded — the SDK
  loads the embedded driver image and serializes g_serve_opts via
  AxlConfig into the driver's LoadOptions, where the driver's
  AXL_SERVICE_DRIVER macro decodes them back into its own g_serve_opts.
  Without --detach, runs in-process via axl_service_run on the default
  loop with an ESC key handler for graceful exit.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include <axl.h>
#include <axl/axl-service.h>

#include "serve/serve-shared.h"
#include "webfs-version.h"

/* Embedded axl-webfs-serve-dxe.efi blob -- emitted by serve-blob.S
   via .incbin. The bytes between the two symbols are the driver
   image; length is the runtime pointer-difference. */
extern const unsigned char axl_embedded_axl_webfs_serve_dxe[];
extern const unsigned char axl_embedded_axl_webfs_serve_dxe_end[];

// ----------------------------------------------------------------------------
// ESC key handler (foreground only)
// ----------------------------------------------------------------------------

static AxlLoop *m_serve_loop = NULL;

static bool
esc_key_handler(AxlInputKey key, void *data)
{
    (void)data;

    if (key.scan_code == 0x17) {  /* SCAN_ESC */
        axl_printf("\nESC -- stopping server.\n");
        if (m_serve_loop != NULL) {
            axl_loop_quit(m_serve_loop);
        }
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Serve verb
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
    { .name = "source",                        .type = AXL_ARG_STRING,
      .help = "Bind listener to interface with this station IPv4 (auto if unset)" },
    { .name = "detach",     .short_name = 'd', .type = AXL_ARG_BOOL,
      .help = "Run as a resident DXE driver and return to the shell" },
    {0}
};

int
webfs_serve_handler(AxlArgs *a)
{
    /* AxlArgs is the source of truth for the foreground binary --
       defaults live in webfs_serve_flags[] and were applied during
       parse. Just write them through into g_serve_opts; AxlConfig
       round-tripping happens on the driver side via the LoadOptions
       handoff inside axl_service_launch_embedded. */
    g_serve_opts.port             = axl_args_get_uint(a, "port");
    g_serve_opts.nic_index        = axl_args_get_string(a, "nic") != NULL
                                  ? axl_args_get_uint(a, "nic")
                                  : (uint64_t)-1;
    g_serve_opts.idle_timeout_sec = axl_args_get_uint(a, "timeout");
    g_serve_opts.verbose          = axl_args_get_bool(a, "verbose");
    g_serve_opts.mode             = axl_args_get_string(a, "mode");
    g_serve_opts.source           = axl_args_get_string(a, "source");

    /* Detach: load the embedded driver image with our options
       serialized into LoadOptions. The driver's AXL_SERVICE_DRIVER
       trampoline decodes them back into its own g_serve_opts and
       calls webfs_serve.setup against a driver-mode loop. */
    if (axl_args_get_bool(a, "detach")) {
        AxlServiceDeploy deploy = {
            .service     = &webfs_serve,
            .driver_blob = axl_embedded_axl_webfs_serve_dxe,
            .driver_name = "axl-webfs-serve-dxe.efi",
        };
        deploy.driver_blob_len =
            (size_t)(axl_embedded_axl_webfs_serve_dxe_end -
                     axl_embedded_axl_webfs_serve_dxe);

        if (axl_service_is_running(&deploy)) {
            axl_printf("axl-webfs: serve already running\n");
            return 0;
        }

        int rc = axl_service_launch_embedded(&deploy);
        if (rc != AXL_OK) {
            axl_printf("ERROR: serve --detach: launch failed (rc=%d)\n", rc);
            return 1;
        }
        return 0;
    }

    /* Foreground: run on the default loop with ESC to quit. */
    AxlLoop *loop = axl_loop_default();
    if (loop == NULL) {
        axl_printf("ERROR: default loop unavailable\n");
        return 1;
    }

    /* Pre-banner -- the post-setup banner ("listening on..." +
       volumes) is printed by serve_setup itself once ft_init has
       enumerated the volumes. */
    axl_printf("\naxl-webfs v%s -- UEFI HTTP File Server\n",
               AXL_WEBFS_VERSION);
    axl_printf("Press ESC to stop.\n\n");

    m_serve_loop = loop;
    axl_loop_add_key_press(loop, esc_key_handler, NULL);

    int rc = axl_service_run(loop, &webfs_serve);

    m_serve_loop = NULL;
    /* axl_service_run returns 0 on clean quit, -1 on Ctrl-C / ESC. */
    return (rc == 0 || rc == -1) ? 0 : 1;
}
