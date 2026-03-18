/** @file
  NetworkLib — NIC discovery, DHCP configuration, and static IP setup.

  Adapted from SoftBMC Core/Network.c. Simplified for UefiXfer: no driver
  loading, no DNS, no connect-all (Shell already connected drivers).
  Single-NIC init with IP4Config2 DHCP + DHCP4 direct fallback.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/NetworkLib.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>

#include <Protocol/SimpleNetwork.h>
#include <Protocol/Ip4Config2.h>
#include <Protocol/Dhcp4.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>

// ----------------------------------------------------------------------------
// Module state
// ----------------------------------------------------------------------------

static BOOLEAN                mInitialized = FALSE;
static EFI_HANDLE             mImageHandle = NULL;
static NETWORK_INTERFACE_INFO mIfaceInfo;

// DHCP4 direct fallback state
static BOOLEAN                mDhcp4Fallback = FALSE;
static EFI_HANDLE             mDhcp4SbHandle = NULL;
static EFI_HANDLE             mDhcp4ChildHandle = NULL;

// Selected NIC handle (for TCP4 ServiceBinding lookup)
static EFI_HANDLE             mNicHandle = NULL;

// ----------------------------------------------------------------------------
// IP4Config2 helpers
// ----------------------------------------------------------------------------

/// Check if a handle already has a non-zero IP via IP4Config2.
static EFI_STATUS CheckExistingIp(
    IN  EFI_HANDLE                 Handle,
    OUT NETWORK_INTERFACE_INFO     *Info
) {
    EFI_IP4_CONFIG2_PROTOCOL *Ip4Cfg2 = NULL;
    EFI_STATUS Status = gBS->OpenProtocol(
        Handle, &gEfiIp4Config2ProtocolGuid, (VOID **)&Ip4Cfg2,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) return Status;

    UINTN DataSize = 0;
    Status = Ip4Cfg2->GetData(
        Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL || DataSize == 0) {
        return EFI_NOT_FOUND;
    }

    EFI_IP4_CONFIG2_INTERFACE_INFO *IfInfo = AllocatePool(DataSize);
    if (IfInfo == NULL) return EFI_OUT_OF_RESOURCES;

    Status = Ip4Cfg2->GetData(
        Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, IfInfo);
    if (EFI_ERROR(Status)) {
        FreePool(IfInfo);
        return Status;
    }

    // Check for non-zero IP
    if ((IfInfo->StationAddress.Addr[0] | IfInfo->StationAddress.Addr[1] |
         IfInfo->StationAddress.Addr[2] | IfInfo->StationAddress.Addr[3]) == 0) {
        FreePool(IfInfo);
        return EFI_NOT_FOUND;
    }

    SetMem(Info, sizeof(*Info), 0);
    Info->Valid = TRUE;
    Info->NicHandle = Handle;
    CopyMem(&Info->Ip, &IfInfo->StationAddress, sizeof(EFI_IPv4_ADDRESS));
    CopyMem(&Info->SubnetMask, &IfInfo->SubnetMask, sizeof(EFI_IPv4_ADDRESS));
    CopyMem(&Info->Mac, &IfInfo->HwAddress, sizeof(EFI_MAC_ADDRESS));
    Info->MacLen = IfInfo->HwAddressSize;
    FreePool(IfInfo);

    // Query gateway (separate data type)
    UINTN GwSize = 0;
    Status = Ip4Cfg2->GetData(
        Ip4Cfg2, Ip4Config2DataTypeGateway, &GwSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL && GwSize >= sizeof(EFI_IPv4_ADDRESS)) {
        EFI_IPv4_ADDRESS *Gw = AllocatePool(GwSize);
        if (Gw != NULL) {
            Status = Ip4Cfg2->GetData(
                Ip4Cfg2, Ip4Config2DataTypeGateway, &GwSize, Gw);
            if (!EFI_ERROR(Status)) {
                CopyMem(&Info->Gateway, Gw, sizeof(EFI_IPv4_ADDRESS));
            }
            FreePool(Gw);
        }
    }

    return EFI_SUCCESS;
}

/// Set DHCP policy on a handle and wait for an IP.
static EFI_STATUS ConfigureDhcpViaIp4Config2(
    IN  EFI_HANDLE  Handle,
    IN  UINTN       TimeoutSeconds
) {
    EFI_IP4_CONFIG2_PROTOCOL *Ip4Cfg2 = NULL;
    EFI_STATUS Status = gBS->OpenProtocol(
        Handle, &gEfiIp4Config2ProtocolGuid, (VOID **)&Ip4Cfg2,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) return Status;

    EFI_IP4_CONFIG2_POLICY DhcpPolicy = Ip4Config2PolicyDhcp;
    Status = Ip4Cfg2->SetData(
        Ip4Cfg2, Ip4Config2DataTypePolicy,
        sizeof(EFI_IP4_CONFIG2_POLICY), &DhcpPolicy);
    if (EFI_ERROR(Status)) return Status;

    // Poll for IP assignment
    for (UINTN Sec = 0; Sec < TimeoutSeconds; Sec++) {
        gBS->Stall(1000000);  // 1 second

        UINTN DataSize = 0;
        Status = Ip4Cfg2->GetData(
            Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, NULL);
        if (Status != EFI_BUFFER_TOO_SMALL || DataSize == 0) continue;

        EFI_IP4_CONFIG2_INTERFACE_INFO *Info = AllocatePool(DataSize);
        if (Info == NULL) continue;

        Status = Ip4Cfg2->GetData(
            Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, Info);
        if (!EFI_ERROR(Status) &&
            (Info->StationAddress.Addr[0] | Info->StationAddress.Addr[1] |
             Info->StationAddress.Addr[2] | Info->StationAddress.Addr[3]) != 0) {
            FreePool(Info);
            return EFI_SUCCESS;
        }
        FreePool(Info);
    }

    return EFI_TIMEOUT;
}

/// Configure a static IP on a handle via IP4Config2.
static EFI_STATUS ConfigureStaticIp(
    IN EFI_HANDLE              Handle,
    IN CONST EFI_IPv4_ADDRESS  *StaticIp
) {
    EFI_IP4_CONFIG2_PROTOCOL *Ip4Cfg2 = NULL;
    EFI_STATUS Status = gBS->OpenProtocol(
        Handle, &gEfiIp4Config2ProtocolGuid, (VOID **)&Ip4Cfg2,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) return Status;

    // Set static policy first
    EFI_IP4_CONFIG2_POLICY StaticPolicy = Ip4Config2PolicyStatic;
    Status = Ip4Cfg2->SetData(
        Ip4Cfg2, Ip4Config2DataTypePolicy,
        sizeof(EFI_IP4_CONFIG2_POLICY), &StaticPolicy);
    if (EFI_ERROR(Status)) return Status;

    // Set manual address with /24 default subnet
    EFI_IP4_CONFIG2_MANUAL_ADDRESS ManualAddr;
    SetMem(&ManualAddr, sizeof(ManualAddr), 0);
    CopyMem(&ManualAddr.Address, StaticIp, sizeof(EFI_IPv4_ADDRESS));
    ManualAddr.SubnetMask.Addr[0] = 255;
    ManualAddr.SubnetMask.Addr[1] = 255;
    ManualAddr.SubnetMask.Addr[2] = 255;
    ManualAddr.SubnetMask.Addr[3] = 0;

    Status = Ip4Cfg2->SetData(
        Ip4Cfg2, Ip4Config2DataTypeManualAddress,
        sizeof(ManualAddr), &ManualAddr);
    return Status;
}

// ----------------------------------------------------------------------------
// DHCP4 direct fallback (when IP4Config2 is not available)
// ----------------------------------------------------------------------------

static EFI_STATUS ConfigureDhcp4Direct(
    IN UINTN TimeoutSeconds
) {
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;

    EFI_STATUS Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiDhcp4ServiceBindingProtocolGuid, NULL,
        &HandleCount, &Handles);
    if (EFI_ERROR(Status) || HandleCount == 0) {
        if (Handles) FreePool(Handles);
        return EFI_NOT_FOUND;
    }

    Print(L"  No IP4Config2 — trying DHCP4 direct...\n");

    for (UINTN i = 0; i < HandleCount; i++) {
        EFI_SERVICE_BINDING_PROTOCOL *Dhcp4Sb = NULL;
        Status = gBS->HandleProtocol(
            Handles[i], &gEfiDhcp4ServiceBindingProtocolGuid, (VOID **)&Dhcp4Sb);
        if (EFI_ERROR(Status) || Dhcp4Sb == NULL) continue;

        EFI_HANDLE ChildHandle = NULL;
        Status = Dhcp4Sb->CreateChild(Dhcp4Sb, &ChildHandle);
        if (EFI_ERROR(Status)) continue;

        EFI_DHCP4_PROTOCOL *Dhcp4 = NULL;
        Status = gBS->HandleProtocol(
            ChildHandle, &gEfiDhcp4ProtocolGuid, (VOID **)&Dhcp4);
        if (EFI_ERROR(Status)) {
            Dhcp4Sb->DestroyChild(Dhcp4Sb, ChildHandle);
            continue;
        }

        UINT32 DiscoverTimeout[] = { 4, 8, 16 };
        UINT32 RequestTimeout[] = { 4, 8 };
        EFI_DHCP4_CONFIG_DATA Cfg;
        ZeroMem(&Cfg, sizeof(Cfg));
        Cfg.DiscoverTryCount = 3;
        Cfg.DiscoverTimeout = DiscoverTimeout;
        Cfg.RequestTryCount = 2;
        Cfg.RequestTimeout = RequestTimeout;

        Status = Dhcp4->Configure(Dhcp4, &Cfg);
        if (EFI_ERROR(Status)) {
            Dhcp4Sb->DestroyChild(Dhcp4Sb, ChildHandle);
            continue;
        }

        Print(L"  DHCP4 discovering...\n");
        Status = Dhcp4->Start(Dhcp4, NULL);
        if (EFI_ERROR(Status)) {
            Dhcp4->Configure(Dhcp4, NULL);
            Dhcp4Sb->DestroyChild(Dhcp4Sb, ChildHandle);
            continue;
        }

        EFI_DHCP4_MODE_DATA ModeData;
        Status = Dhcp4->GetModeData(Dhcp4, &ModeData);
        if (EFI_ERROR(Status)) {
            Dhcp4->Stop(Dhcp4);
            Dhcp4->Configure(Dhcp4, NULL);
            Dhcp4Sb->DestroyChild(Dhcp4Sb, ChildHandle);
            continue;
        }

        // Store results
        mDhcp4Fallback = TRUE;
        mDhcp4SbHandle = Handles[i];
        mDhcp4ChildHandle = ChildHandle;

        SetMem(&mIfaceInfo, sizeof(mIfaceInfo), 0);
        mIfaceInfo.Valid = TRUE;
        mIfaceInfo.NicHandle = Handles[i];
        CopyMem(&mIfaceInfo.Ip, &ModeData.ClientAddress, sizeof(EFI_IPv4_ADDRESS));
        CopyMem(&mIfaceInfo.SubnetMask, &ModeData.SubnetMask, sizeof(EFI_IPv4_ADDRESS));
        CopyMem(&mIfaceInfo.Gateway, &ModeData.RouterAddress, sizeof(EFI_IPv4_ADDRESS));
        CopyMem(&mIfaceInfo.Mac, &ModeData.ClientMacAddress, sizeof(EFI_MAC_ADDRESS));
        mIfaceInfo.MacLen = 6;
        mNicHandle = Handles[i];

        FreePool(Handles);
        return EFI_SUCCESS;
    }

    FreePool(Handles);
    return EFI_NOT_FOUND;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

EFI_STATUS
EFIAPI
NetworkInit (
    IN EFI_HANDLE              ImageHandle,
    IN UINTN                   NicIndex,
    IN EFI_IPv4_ADDRESS        *StaticIp       OPTIONAL,
    IN UINTN                   TimeoutSeconds
    )
{
    if (mInitialized) return EFI_ALREADY_STARTED;

    mImageHandle = ImageHandle;
    SetMem(&mIfaceInfo, sizeof(mIfaceInfo), 0);

    // Enumerate IP4Config2 handles (these correspond to NICs with network stack)
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;
    EFI_STATUS Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL,
        &HandleCount, &Handles);

    if (EFI_ERROR(Status) || HandleCount == 0) {
        if (Handles) FreePool(Handles);
        // No IP4Config2 — try DHCP4 direct fallback
        Status = ConfigureDhcp4Direct(TimeoutSeconds);
        if (EFI_ERROR(Status)) {
            Print(L"ERROR: No network interface found\n");
            return EFI_NOT_FOUND;
        }
        mInitialized = TRUE;
        return EFI_SUCCESS;
    }

    // Select NIC by index or first available
    UINTN SelectedIdx = 0;
    if (NicIndex != (UINTN)-1) {
        if (NicIndex >= HandleCount) {
            Print(L"ERROR: NIC index %d out of range (0-%d)\n",
                  NicIndex, HandleCount - 1);
            FreePool(Handles);
            return EFI_NOT_FOUND;
        }
        SelectedIdx = NicIndex;
    }

    EFI_HANDLE SelectedHandle = Handles[SelectedIdx];
    mNicHandle = SelectedHandle;
    FreePool(Handles);

    // Static IP requested?
    if (StaticIp != NULL) {
        Status = ConfigureStaticIp(SelectedHandle, StaticIp);
        if (EFI_ERROR(Status)) {
            Print(L"ERROR: Failed to set static IP: %r\n", Status);
            return Status;
        }
        // Brief wait for the stack to apply the config
        gBS->Stall(500000);
    }

    // Check if NIC already has an IP
    Status = CheckExistingIp(SelectedHandle, &mIfaceInfo);
    if (!EFI_ERROR(Status)) {
        mInitialized = TRUE;
        return EFI_SUCCESS;
    }

    // No IP yet — run DHCP via IP4Config2
    Print(L"  DHCP on interface %d...\n", SelectedIdx);
    Status = ConfigureDhcpViaIp4Config2(SelectedHandle, TimeoutSeconds);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: DHCP timeout after %ds\n", TimeoutSeconds);
        return EFI_TIMEOUT;
    }

    // Re-read the assigned IP
    Status = CheckExistingIp(SelectedHandle, &mIfaceInfo);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: DHCP completed but no IP assigned\n");
        return EFI_DEVICE_ERROR;
    }

    mInitialized = TRUE;
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
NetworkGetAddress (
    OUT EFI_IPv4_ADDRESS  *Addr
    )
{
    if (!mInitialized || !mIfaceInfo.Valid) return EFI_NOT_READY;
    CopyMem(Addr, &mIfaceInfo.Ip, sizeof(EFI_IPv4_ADDRESS));
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
NetworkGetInterfaceInfo (
    OUT NETWORK_INTERFACE_INFO  *Info
    )
{
    if (!mInitialized || !mIfaceInfo.Valid) return EFI_NOT_READY;
    CopyMem(Info, &mIfaceInfo, sizeof(NETWORK_INTERFACE_INFO));
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
NetworkGetTcpServiceBinding (
    OUT EFI_HANDLE  *SbHandle
    )
{
    if (!mInitialized) return EFI_NOT_READY;
    if (SbHandle == NULL) return EFI_INVALID_PARAMETER;

    // Find TCP4 ServiceBinding on the selected NIC handle or its children.
    // Try the NIC handle first, then scan all TCP4 SB handles.
    EFI_STATUS Status = gBS->OpenProtocol(
        mNicHandle, &gEfiTcp4ServiceBindingProtocolGuid, NULL,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_TEST_PROTOCOL);
    if (!EFI_ERROR(Status)) {
        *SbHandle = mNicHandle;
        return EFI_SUCCESS;
    }

    // TCP4 SB may be on a child handle — find the first one
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;
    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiTcp4ServiceBindingProtocolGuid, NULL,
        &HandleCount, &Handles);
    if (EFI_ERROR(Status) || HandleCount == 0) {
        if (Handles) FreePool(Handles);
        return EFI_NOT_FOUND;
    }

    *SbHandle = Handles[0];
    FreePool(Handles);
    return EFI_SUCCESS;
}

VOID
EFIAPI
NetworkCleanup (
    VOID
    )
{
    if (mDhcp4Fallback && mDhcp4ChildHandle != NULL) {
        EFI_DHCP4_PROTOCOL *Dhcp4 = NULL;
        gBS->HandleProtocol(
            mDhcp4ChildHandle, &gEfiDhcp4ProtocolGuid, (VOID **)&Dhcp4);
        if (Dhcp4 != NULL) {
            Dhcp4->Stop(Dhcp4);
            Dhcp4->Configure(Dhcp4, NULL);
        }

        EFI_SERVICE_BINDING_PROTOCOL *Dhcp4Sb = NULL;
        gBS->HandleProtocol(
            mDhcp4SbHandle, &gEfiDhcp4ServiceBindingProtocolGuid, (VOID **)&Dhcp4Sb);
        if (Dhcp4Sb != NULL) {
            Dhcp4Sb->DestroyChild(Dhcp4Sb, mDhcp4ChildHandle);
        }

        mDhcp4ChildHandle = NULL;
        mDhcp4SbHandle = NULL;
        mDhcp4Fallback = FALSE;
    }

    SetMem(&mIfaceInfo, sizeof(mIfaceInfo), 0);
    mNicHandle = NULL;
    mImageHandle = NULL;
    mInitialized = FALSE;
}
