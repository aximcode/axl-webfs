/** @file
  axl-webfs-serve-dxe -- thin DXE driver wrapper around the shared
  AxlService descriptor in serve-core.c.

  AXL_SERVICE_DRIVER expands to the firmware-side DriverEntry +
  Unload stubs that decode LoadOptions into ServeOpts via serve_descs,
  call webfs_serve.setup, register a periodic tick (from
  webfs_serve.driver_tick_ms) that drives the loop via
  axl_loop_attach_driver, and reverse the dance at Unload time.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include <axl.h>

#include "serve/serve-shared.h"

AXL_LOG_DOMAIN("webfs-serve-drv");

AXL_SERVICE_DRIVER(webfs_serve)
