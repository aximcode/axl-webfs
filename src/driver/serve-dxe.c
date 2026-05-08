/** @file
  axl-webfs-serve-dxe -- resident HTTP-server DXE driver.

  Brought up by `axl-webfs serve --detach` from the shell. The driver
  parses load options, calls serve_core_setup, registers a
  `webfs-server` protocol on a fresh handle, and asks the SDK to drive
  the AxlLoop in DXE-driver mode (axl_loop_attach_driver). DriverEntry
  returns AXL_OK so the shell stays interactive; firmware-managed
  notify timer ticks pump the loop until the matching unload runs.

  The HTTP plumbing -- routes, middleware, server bring-up -- is
  shared with the foreground `serve` command via src/serve/serve-core.

  Pure tier-1 AXL driver: no EFI_* / EFIAPI / <uefi/axl-uefi.h>.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include <axl.h>

#include "serve/serve-core.h"
#include "serve/serve-protocol.h"
#include "webfs-version.h"

AXL_LOG_DOMAIN("webfs-serve-drv");

/* AxlLoop driver-mode tick interval. The SDK's notify-budget rule is
   ~1 ms per source callback; 50 ms gives co-located TCP4/MNP/SNP
   plenty of room between our ticks. */
#define SERVE_TICK_MS  50

static const AxlGuid       gWebfsServerGuid = WEBFS_SERVER_PROTOCOL_GUID;

static ServeCore           m_core;
static void               *m_protocol_handle = NULL;
static WebfsServerProtocol m_protocol;
static char                m_source_buf[16];
static bool                m_loop_attached = false;

// ----------------------------------------------------------------------------
// Forward declarations the AXL_DRIVER macro expects.
// ----------------------------------------------------------------------------

static int serve_main(AxlHandle image, AxlSystemTable *st);
static int serve_unload(AxlHandle image);

AXL_DRIVER(serve_main, serve_unload)

// ----------------------------------------------------------------------------
// Driver unload
// ----------------------------------------------------------------------------

static int
serve_unload(AxlHandle image)
{
    (void)image;

    /* Stop the firmware-managed dispatch timer FIRST so no further
       loop iterations fire concurrently with the teardown. */
    if (m_loop_attached) {
        axl_loop_detach_driver(m_core.loop);
        m_loop_attached = false;
    }

    if (m_protocol_handle != NULL) {
        axl_protocol_unregister(m_protocol_handle,
                                WEBFS_SERVER_PROTOCOL_NAME, &m_protocol);
        m_protocol_handle = NULL;
    }

    serve_core_teardown(&m_core);

    axl_printf("axl-webfs: serve stopped.\n");
    axl_info("unloaded");
    return AXL_OK;
}

// ----------------------------------------------------------------------------
// Driver entry
// ----------------------------------------------------------------------------

static int
serve_main(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;

    axl_info("loading");

    /* --- Parse load options ---------------------------------------- */
    const char *load_opts = axl_driver_get_load_options();
    if (load_opts == NULL) {
        axl_printf("ERROR: axl-webfs-serve-dxe: missing load options\n");
        return AXL_ERR;
    }

    ServeCoreOpts opts;
    if (serve_opts_parse(load_opts, &opts,
                         m_source_buf, sizeof(m_source_buf)) != AXL_OK)
    {
        axl_printf("ERROR: axl-webfs-serve-dxe: malformed load options\n");
        return AXL_ERR;
    }

    /* --- Bring the HTTP server up via the shared core -------------- */
    if (serve_core_setup(&opts, &m_core) != AXL_OK) {
        axl_printf("ERROR: axl-webfs-serve-dxe: serve bring-up failed\n");
        return AXL_ERR;
    }

    /* --- Pin the custom GUID and register the management protocol -- */
    if (axl_protocol_register_name(WEBFS_SERVER_PROTOCOL_NAME,
                                   &gWebfsServerGuid) != AXL_OK)
    {
        axl_printf("ERROR: axl-webfs-serve-dxe: protocol-name pin failed\n");
        goto fail_after_core;
    }

    m_protocol.version = WEBFS_SERVER_PROTOCOL_VERSION;
    m_protocol.port    = opts.port;
    const char *mode_str = opts.read_only  ? "read-only"
                         : opts.write_only ? "write-only"
                         :                   "read-write";
    m_protocol.mode[0] = '\0';
    for (size_t i = 0;
         i < sizeof(m_protocol.mode) - 1 && mode_str[i] != '\0';
         i++)
    {
        m_protocol.mode[i]     = mode_str[i];
        m_protocol.mode[i + 1] = '\0';
    }
    m_protocol.addr[0] = m_core.addr[0];
    m_protocol.addr[1] = m_core.addr[1];
    m_protocol.addr[2] = m_core.addr[2];
    m_protocol.addr[3] = m_core.addr[3];

    if (axl_protocol_register(WEBFS_SERVER_PROTOCOL_NAME,
                              &m_protocol, &m_protocol_handle) != AXL_OK
        || m_protocol_handle == NULL)
    {
        axl_printf("ERROR: axl-webfs-serve-dxe: protocol install failed\n");
        goto fail_after_core;
    }

    /* --- Hand the loop to the SDK's driver-mode dispatcher --------- */
    if (axl_loop_attach_driver(m_core.loop, SERVE_TICK_MS) != AXL_OK) {
        axl_printf("ERROR: axl-webfs-serve-dxe: loop attach_driver failed\n");
        goto fail_after_register;
    }
    m_loop_attached = true;

    axl_printf("\naxl-webfs v%s -- HTTP file server (background)\n",
               AXL_WEBFS_VERSION);
    axl_printf("Listening on %d.%d.%d.%d:%d (mode %s)\n",
               m_core.addr[0], m_core.addr[1], m_core.addr[2], m_core.addr[3],
               opts.port, mode_str);
    axl_printf("Use `axl-webfs serve-stop` to shut down.\n\n");

    return AXL_OK;

fail_after_register:
    axl_protocol_unregister(m_protocol_handle,
                            WEBFS_SERVER_PROTOCOL_NAME, &m_protocol);
    m_protocol_handle = NULL;
fail_after_core:
    serve_core_teardown(&m_core);
    return AXL_ERR;
}
