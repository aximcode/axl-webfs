/** @file
  axl-webfs -- `serve` and `serve-stop` verbs.

  serve: populates g_serve_opts from AxlArgs (the launcher and driver
  agree on the descriptor via serve_descs, so the launcher's manual
  fill matches what AXL_SERVICE_DRIVER decodes on the driver side
  from LoadOptions). Hands an AxlServiceDeploy to
  axl_service_start_embedded to load the embedded
  webfs-serve-dxe.efi, then returns to the shell. The HTTP server
  runs as a resident DXE driver until `serve-stop` (or `unload -n
  axl-webfs-serve-dxe.efi`).

  serve-stop: calls axl_service_stop, which resolves the running
  driver image by the service's name-derived GUID and unloads it.
  Idempotent (handles "not running" cleanly).

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include <axl.h>
#include <axl/axl-embed.h>
#include <axl/axl-service.h>

#include "serve/webfs-serve.h"

/* Embedded axl-webfs-serve-dxe.efi blob -- spliced in by
   `axl-cc --embed ...=axl_webfs_serve_dxe` (see Makefile). */
AXL_EMBED_DECLARE(axl_webfs_serve_dxe);

// ----------------------------------------------------------------------------
// Serve verb
// ----------------------------------------------------------------------------

const AxlArgDesc webfs_serve_flags[] = {
    { .name = "port",       .short_name = 'p', .type = AXL_ARG_U16,
      .default_value = "9876", .help = "Listen port" },
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
    { .name = "listen-ip",                     .type = AXL_ARG_STRING,
      .help = "Bind listener to interface with this station IPv4 (auto if unset)" },
    { .name = "log",        .short_name = 'l', .type = AXL_ARG_STRING,
      .help = "Log file path (e.g. fs0:\\webfs.log; default: console only)" },
    { .name = "auth",       .short_name = 'a', .type = AXL_ARG_STRING,
      .help = "Require HTTP Basic auth: user:pass (gates all surfaces; "
              "default: open)" },
    {0}
};

// ----------------------------------------------------------------------------
// serve-stop verb -- symmetric counterpart to serve.
// ----------------------------------------------------------------------------

int
webfs_serve_stop_handler(AxlArgs *a)
{
    (void)a;

    /* axl_service_stop only reads deploy->service (the name-derived
       GUID is what it actually looks up); driver_blob / driver_name
       aren't needed (no relaunch path). */
    AxlServiceDeploy deploy = {
        .service = &webfs_serve,
    };

    if (!axl_service_is_running(&deploy)) {
        axl_printf("axl-webfs: no serve running\n");
        return 0;
    }

    axl_printf("axl-webfs: stopping serve...\n");
    int rc = axl_service_stop(&deploy);
    if (rc != AXL_OK) {
        axl_printf("ERROR: serve-stop failed (rc=%d)\n", rc);
        return 1;
    }

    axl_printf("axl-webfs: serve stopped\n");
    return 0;
}

int
webfs_serve_handler(AxlArgs *a)
{
    g_serve_opts.net.port         = (uint16_t)axl_args_get_uint(a, "port");
    g_serve_opts.net.nic_index    = axl_args_get_string(a, "nic") != NULL
                                  ? axl_args_get_uint(a, "nic")
                                  : AXL_NET_NIC_AUTO;
    g_serve_opts.net.local_ip     = axl_args_get_string(a, "listen-ip");
    if (g_serve_opts.net.local_ip == NULL)
        g_serve_opts.net.local_ip = "";
    g_serve_opts.idle_timeout_sec = axl_args_get_uint(a, "timeout");
    g_serve_opts.verbose          = axl_args_get_bool(a, "verbose");
    g_serve_opts.mode             = axl_args_get_string(a, "mode");
    g_serve_opts.log_path         = axl_args_get_string(a, "log");
    g_serve_opts.auth             = axl_args_get_string(a, "auth");
    if (g_serve_opts.auth == NULL)
        g_serve_opts.auth = "";
    /* A colon-less --auth value can never match an HTTP Basic
       credential (which always decodes to "user:pass"), so it would
       silently lock out every client -- including the operator.
       Reject it here with a clear message instead of starting an
       unreachable server. */
    if (g_serve_opts.auth[0] != '\0' &&
        axl_strchr(g_serve_opts.auth, ':') == NULL) {
        axl_printf("ERROR: serve: --auth must be \"user:pass\" "
                   "(missing ':')\n");
        return 1;
    }

    AxlServiceDeploy deploy = {
        .service     = &webfs_serve,
        .driver_blob = AXL_EMBED_DATA(axl_webfs_serve_dxe),
        .driver_name = "axl-webfs-serve-dxe.efi",
    };
    deploy.driver_blob_len = AXL_EMBED_SIZE(axl_webfs_serve_dxe);

    if (axl_service_is_running(&deploy)) {
        axl_printf("axl-webfs: serve already running\n");
        return 0;
    }

    int rc = axl_service_start_embedded(&deploy);
    if (rc != AXL_OK) {
        axl_printf("ERROR: serve: start failed (rc=%d)\n", rc);
        return 1;
    }
    return 0;
}
