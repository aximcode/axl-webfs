/** @file
  NetworkLib — NIC discovery, DHCP configuration, and static IP setup.

  Provides a single-call initialization that enumerates network interfaces,
  selects a NIC (by index or first available), and configures an IP address
  via DHCP or static assignment. Returns the TCP4 ServiceBinding handle for
  use by HttpClientLib/HttpServerLib.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef NETWORK_LIB_H_
#define NETWORK_LIB_H_

#include <Uefi.h>
#include <Protocol/Ip4Config2.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/Tcp4.h>

/// Information about the configured network interface.
typedef struct {
    EFI_IPv4_ADDRESS  Ip;
    EFI_IPv4_ADDRESS  SubnetMask;
    EFI_IPv4_ADDRESS  Gateway;
    EFI_MAC_ADDRESS   Mac;
    UINT32            MacLen;
    BOOLEAN           Valid;
    EFI_HANDLE        NicHandle;
} NETWORK_INTERFACE_INFO;

/**
  Initialize networking on a single NIC.

  Enumerates NICs, selects one (by NicIndex or first available), and
  configures an IP address via DHCP or static assignment.

  @param[in]  ImageHandle     The image handle of the caller.
  @param[in]  NicIndex        NIC index to use, or (UINTN)-1 for first available.
  @param[in]  StaticIp        Static IP to configure, or NULL for DHCP.
  @param[in]  TimeoutSeconds  DHCP timeout in seconds (default 10).

  @retval EFI_SUCCESS         Network initialized, IP address available.
  @retval EFI_NOT_FOUND       No NIC found.
  @retval EFI_TIMEOUT         DHCP did not complete within timeout.
  @retval EFI_DEVICE_ERROR    NIC or protocol error.
**/
EFI_STATUS
EFIAPI
NetworkInit (
    IN EFI_HANDLE              ImageHandle,
    IN UINTN                   NicIndex,
    IN EFI_IPv4_ADDRESS        *StaticIp       OPTIONAL,
    IN UINTN                   TimeoutSeconds
    );

/**
  Get the configured IP address.

  @param[out] Addr  The IPv4 address of the configured NIC.

  @retval EFI_SUCCESS         Address returned.
  @retval EFI_NOT_READY       NetworkInit() has not been called.
**/
EFI_STATUS
EFIAPI
NetworkGetAddress (
    OUT EFI_IPv4_ADDRESS  *Addr
    );

/**
  Get full interface information.

  @param[out] Info  Populated with IP, subnet, gateway, MAC, and NIC handle.

  @retval EFI_SUCCESS         Info returned.
  @retval EFI_NOT_READY       NetworkInit() has not been called.
**/
EFI_STATUS
EFIAPI
NetworkGetInterfaceInfo (
    OUT NETWORK_INTERFACE_INFO  *Info
    );

/**
  Get the TCP4 ServiceBinding handle for the configured NIC.

  Used by HttpClientLib and HttpServerLib to create TCP child handles
  on the correct network interface.

  @param[out] SbHandle  Handle with EFI_TCP4_SERVICE_BINDING_PROTOCOL.

  @retval EFI_SUCCESS     Handle returned.
  @retval EFI_NOT_READY   NetworkInit() has not been called.
**/
EFI_STATUS
EFIAPI
NetworkGetTcpServiceBinding (
    OUT EFI_HANDLE  *SbHandle
    );

/**
  Release network resources.

  Destroys any DHCP4 child created during init and clears state.
**/
VOID
EFIAPI
NetworkCleanup (
    VOID
    );

#endif // NETWORK_LIB_H_
