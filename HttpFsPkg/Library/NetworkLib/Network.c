/** @file
  NetworkLib — NIC discovery, DHCP configuration, and static IP setup.

  Adapted from SoftBMC Core/Network.c. Auto-loads NIC drivers from
  \drivers\{arch}\ on mounted filesystems, connects network stack
  drivers, and auto-selects the first NIC with an IP. Single-NIC init
  with IP4Config2 DHCP + DHCP4 direct fallback.

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
#include <Library/DevicePathLib.h>

#include <Protocol/SimpleNetwork.h>
#include <Protocol/Ip4Config2.h>
#include <Protocol/Dhcp4.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>

#if defined(MDE_CPU_X64)
  #define DRIVER_DIR_PATH  L"\\drivers\\x64"
#elif defined(MDE_CPU_AARCH64)
  #define DRIVER_DIR_PATH  L"\\drivers\\aa64"
#else
  #define DRIVER_DIR_PATH  L"\\drivers"
#endif

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
// Link status check
// ----------------------------------------------------------------------------

/// Check link status via SNP. Returns EFI_NO_MEDIA if cable unplugged.
static EFI_STATUS CheckLinkStatus(
    IN EFI_HANDLE  NicHandle
) {
    EFI_SIMPLE_NETWORK_PROTOCOL *Snp = NULL;
    EFI_STATUS Status = gBS->OpenProtocol(
        NicHandle, &gEfiSimpleNetworkProtocolGuid, (VOID **)&Snp,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status) || Snp == NULL) {
        // SNP not on this handle — skip check
        return EFI_SUCCESS;
    }

    if (Snp->Mode->MediaPresentSupported && !Snp->Mode->MediaPresent) {
        Print(L"WARNING: NIC reports no link (cable unplugged?)\n");
        Print(L"  Some firmware reports link-down incorrectly — continuing anyway.\n");
    }

    return EFI_SUCCESS;
}

// ----------------------------------------------------------------------------
// Auto-load NIC drivers from filesystem
// ----------------------------------------------------------------------------

/// Search mounted filesystems for \drivers\{arch}\*.efi and load them.
/// Adapted from SoftBMC NetworkLoadDrivers(). Stops after the first
/// filesystem that has a drivers directory.
static UINTN LoadNicDrivers(VOID) {
    EFI_STATUS Status;
    EFI_HANDLE *FsHandles = NULL;
    UINTN FsCount = 0;
    UINTN DriversLoaded = 0;

    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL,
        &FsCount, &FsHandles);
    if (EFI_ERROR(Status) || FsCount == 0) {
        if (FsHandles) FreePool(FsHandles);
        return 0;
    }

    for (UINTN i = 0; i < FsCount; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
        EFI_FILE_PROTOCOL *Root = NULL;
        EFI_FILE_PROTOCOL *DirHandle = NULL;

        Status = gBS->OpenProtocol(
            FsHandles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs,
            mImageHandle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status)) continue;

        Status = Fs->OpenVolume(Fs, &Root);
        if (EFI_ERROR(Status)) continue;

        Status = Root->Open(Root, &DirHandle, DRIVER_DIR_PATH, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(Status)) {
            Root->Close(Root);
            continue;
        }

        // Enumerate .efi files in the drivers directory
        UINT8 Buffer[512];
        for (;;) {
            UINTN BufSize = sizeof(Buffer);
            Status = DirHandle->Read(DirHandle, &BufSize, Buffer);
            if (EFI_ERROR(Status) || BufSize == 0) break;

            EFI_FILE_INFO *FileInfo = (EFI_FILE_INFO *)Buffer;
            if (FileInfo->Attribute & EFI_FILE_DIRECTORY) continue;

            UINTN NameLen = StrLen(FileInfo->FileName);
            if (NameLen <= 4) continue;
            if (StrnCmp(&FileInfo->FileName[NameLen - 4], L".efi", 4) != 0 &&
                StrnCmp(&FileInfo->FileName[NameLen - 4], L".EFI", 4) != 0) {
                continue;
            }

            CHAR16 FullPath[256];
            UnicodeSPrint(FullPath, sizeof(FullPath),
                          L"%s\\%s", DRIVER_DIR_PATH, FileInfo->FileName);

            EFI_DEVICE_PATH_PROTOCOL *DevicePath = FileDevicePath(FsHandles[i], FullPath);
            if (DevicePath == NULL) continue;

            EFI_HANDLE DriverHandle = NULL;
            Print(L"  Loading %s ... ", FileInfo->FileName);
            Status = gBS->LoadImage(FALSE, mImageHandle, DevicePath, NULL, 0, &DriverHandle);
            if (!EFI_ERROR(Status)) {
                Status = gBS->StartImage(DriverHandle, NULL, NULL);
                if (!EFI_ERROR(Status)) {
                    DriversLoaded++;
                    Print(L"OK\n");
                } else {
                    Print(L"start failed: %r\n", Status);
                    gBS->UnloadImage(DriverHandle);
                }
            } else {
                Print(L"load failed: %r\n", Status);
            }

            FreePool(DevicePath);
        }

        DirHandle->Close(DirHandle);
        Root->Close(Root);

        if (DriversLoaded > 0) break;
    }

    FreePool(FsHandles);
    return DriversLoaded;
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

        // Check link before DHCP4 discover (warn only — firmware may misreport)
        {
            EFI_SIMPLE_NETWORK_PROTOCOL *Snp = NULL;
            gBS->HandleProtocol(
                Handles[i], &gEfiSimpleNetworkProtocolGuid, (VOID **)&Snp);
            if (Snp != NULL &&
                Snp->Mode->MediaPresentSupported && !Snp->Mode->MediaPresent) {
                Print(L"  NIC %d: reports no link (may be incorrect)\n", i);
            }
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

/// Try to bring up the network stack: connect SNP handles, optionally
/// load drivers from filesystem first. Returns the number of IP4Config2
/// handles found after connecting.
static UINTN BringUpNetworkStack(VOID) {
    EFI_HANDLE *SnpHandles = NULL;
    UINTN SnpCount = 0;
    EFI_STATUS Status;

    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL,
        &SnpCount, &SnpHandles);

    if (EFI_ERROR(Status) || SnpCount == 0) {
        // No SNP at all — try loading NIC drivers from filesystem
        if (SnpHandles) FreePool(SnpHandles);
        UINTN Loaded = LoadNicDrivers();
        if (Loaded > 0) {
            Status = gBS->LocateHandleBuffer(
                ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL,
                &SnpCount, &SnpHandles);
            if (EFI_ERROR(Status) || SnpCount == 0) {
                if (SnpHandles) FreePool(SnpHandles);
                return 0;
            }
        } else {
            return 0;
        }
    }

    // Recursively connect all SNP handles to bring up MNP/ARP/IP4/TCP4
    Print(L"  Connecting network stack on %d NIC(s)...\n", SnpCount);
    for (UINTN i = 0; i < SnpCount; i++) {
        gBS->ConnectController(SnpHandles[i], NULL, NULL, TRUE);
    }
    FreePool(SnpHandles);

    // Count IP4Config2 handles now available
    EFI_HANDLE *Ip4Handles = NULL;
    UINTN Ip4Count = 0;
    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL,
        &Ip4Count, &Ip4Handles);
    if (Ip4Handles) FreePool(Ip4Handles);
    return EFI_ERROR(Status) ? 0 : Ip4Count;
}

/// Find the IP4Config2 handle that corresponds to a given SNP handle.
/// SNP and IP4Config2 may be on the same handle or on parent/child handles
/// sharing the same MAC address.
static EFI_HANDLE FindIp4HandleForSnp(
    IN EFI_HANDLE  SnpHandle
) {
    EFI_STATUS Status;

    // Fast path: IP4Config2 is on the same handle as SNP
    Status = gBS->OpenProtocol(
        SnpHandle, &gEfiIp4Config2ProtocolGuid, NULL,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_TEST_PROTOCOL);
    if (!EFI_ERROR(Status)) {
        return SnpHandle;
    }

    // Slow path: match by MAC address
    EFI_SIMPLE_NETWORK_PROTOCOL *Snp = NULL;
    Status = gBS->OpenProtocol(
        SnpHandle, &gEfiSimpleNetworkProtocolGuid, (VOID **)&Snp,
        mImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status) || Snp == NULL) return NULL;

    EFI_HANDLE *Ip4Handles = NULL;
    UINTN Ip4Count = 0;
    Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL,
        &Ip4Count, &Ip4Handles);
    if (EFI_ERROR(Status) || Ip4Count == 0) {
        if (Ip4Handles) FreePool(Ip4Handles);
        return NULL;
    }

    EFI_HANDLE Match = NULL;
    for (UINTN i = 0; i < Ip4Count; i++) {
        NETWORK_INTERFACE_INFO TmpInfo;
        if (!EFI_ERROR(CheckExistingIp(Ip4Handles[i], &TmpInfo)) ||
            TmpInfo.MacLen > 0) {
            // CheckExistingIp only fills info if IP is non-zero.
            // For MAC-only match, query IP4Config2 directly.
        }
        EFI_IP4_CONFIG2_PROTOCOL *Ip4Cfg2 = NULL;
        Status = gBS->OpenProtocol(
            Ip4Handles[i], &gEfiIp4Config2ProtocolGuid, (VOID **)&Ip4Cfg2,
            mImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(Status)) continue;

        UINTN DataSize = 0;
        Status = Ip4Cfg2->GetData(
            Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, NULL);
        if (Status != EFI_BUFFER_TOO_SMALL || DataSize == 0) continue;

        EFI_IP4_CONFIG2_INTERFACE_INFO *IfInfo = AllocatePool(DataSize);
        if (IfInfo == NULL) continue;

        Status = Ip4Cfg2->GetData(
            Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, IfInfo);
        if (!EFI_ERROR(Status)) {
            if (CompareMem(&IfInfo->HwAddress, &Snp->Mode->CurrentAddress,
                           Snp->Mode->HwAddressSize) == 0) {
                Match = Ip4Handles[i];
                FreePool(IfInfo);
                break;
            }
        }
        FreePool(IfInfo);
    }

    FreePool(Ip4Handles);
    return Match;
}

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

    //
    // Phase 1: Ensure IP4Config2 handles exist.
    // Try direct enumeration first, then connect SNP, then load drivers.
    //
    EFI_HANDLE *Ip4Handles = NULL;
    UINTN Ip4Count = 0;
    EFI_STATUS Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL,
        &Ip4Count, &Ip4Handles);

    if (EFI_ERROR(Status) || Ip4Count == 0) {
        if (Ip4Handles) FreePool(Ip4Handles);
        Ip4Handles = NULL;
        Ip4Count = 0;

        UINTN NewCount = BringUpNetworkStack();
        if (NewCount == 0) {
            // Last resort: DHCP4 direct fallback
            Status = ConfigureDhcp4Direct(TimeoutSeconds);
            if (EFI_ERROR(Status)) {
                Print(L"ERROR: No network interface found\n");
                return EFI_NOT_FOUND;
            }
            mInitialized = TRUE;
            return EFI_SUCCESS;
        }

        // Re-enumerate after bringing up the stack
        Status = gBS->LocateHandleBuffer(
            ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL,
            &Ip4Count, &Ip4Handles);
        if (EFI_ERROR(Status) || Ip4Count == 0) {
            if (Ip4Handles) FreePool(Ip4Handles);
            Status = ConfigureDhcp4Direct(TimeoutSeconds);
            if (EFI_ERROR(Status)) {
                Print(L"ERROR: No network interface found\n");
                return EFI_NOT_FOUND;
            }
            mInitialized = TRUE;
            return EFI_SUCCESS;
        }
    }

    //
    // Phase 2: Select a NIC.
    // NicIndex uses SNP handle ordering (consistent with list-nics).
    // Map it to the corresponding IP4Config2 handle.
    //
    EFI_HANDLE SelectedIp4Handle = NULL;

    if (NicIndex != (UINTN)-1) {
        // User specified -n: use SNP-based indexing (matches list-nics)
        EFI_HANDLE *SnpHandles = NULL;
        UINTN SnpCount = 0;
        Status = gBS->LocateHandleBuffer(
            ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL,
            &SnpCount, &SnpHandles);
        if (EFI_ERROR(Status) || NicIndex >= SnpCount) {
            Print(L"ERROR: NIC index %d out of range (0-%d)\n",
                  NicIndex, EFI_ERROR(Status) ? 0 : SnpCount - 1);
            if (SnpHandles) FreePool(SnpHandles);
            FreePool(Ip4Handles);
            return EFI_NOT_FOUND;
        }
        SelectedIp4Handle = FindIp4HandleForSnp(SnpHandles[NicIndex]);
        if (SelectedIp4Handle == NULL) {
            Print(L"ERROR: NIC %d has no IP4 stack (try 'connect -r' first)\n", NicIndex);
            FreePool(SnpHandles);
            FreePool(Ip4Handles);
            return EFI_NOT_FOUND;
        }
        FreePool(SnpHandles);
    } else {
        // Auto-select: find first NIC that already has an IP
        for (UINTN i = 0; i < Ip4Count; i++) {
            NETWORK_INTERFACE_INFO TmpInfo;
            if (!EFI_ERROR(CheckExistingIp(Ip4Handles[i], &TmpInfo))) {
                SelectedIp4Handle = Ip4Handles[i];
                Print(L"  Found NIC with IP %d.%d.%d.%d\n",
                      TmpInfo.Ip.Addr[0], TmpInfo.Ip.Addr[1],
                      TmpInfo.Ip.Addr[2], TmpInfo.Ip.Addr[3]);
                break;
            }
        }
        // If none have an IP yet, use first handle and DHCP
        if (SelectedIp4Handle == NULL) {
            SelectedIp4Handle = Ip4Handles[0];
        }
    }

    mNicHandle = SelectedIp4Handle;
    FreePool(Ip4Handles);

    // Check link (warning only)
    CheckLinkStatus(SelectedIp4Handle);

    // Static IP requested?
    if (StaticIp != NULL) {
        Status = ConfigureStaticIp(SelectedIp4Handle, StaticIp);
        if (EFI_ERROR(Status)) {
            Print(L"ERROR: Failed to set static IP: %r\n", Status);
            return Status;
        }
        gBS->Stall(500000);
    }

    // Check if NIC already has an IP
    Status = CheckExistingIp(SelectedIp4Handle, &mIfaceInfo);
    if (!EFI_ERROR(Status)) {
        mInitialized = TRUE;
        return EFI_SUCCESS;
    }

    // No IP yet — run DHCP via IP4Config2
    Print(L"  DHCP...\n");
    Status = ConfigureDhcpViaIp4Config2(SelectedIp4Handle, TimeoutSeconds);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: DHCP timeout after %ds\n", TimeoutSeconds);
        return EFI_TIMEOUT;
    }

    // Re-read the assigned IP
    Status = CheckExistingIp(SelectedIp4Handle, &mIfaceInfo);
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

VOID
EFIAPI
NetworkListNics (
    IN EFI_HANDLE  ImageHandle
    )
{
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;

    EFI_STATUS Status = gBS->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL,
        &HandleCount, &Handles);
    if (EFI_ERROR(Status) || HandleCount == 0) {
        if (Handles) FreePool(Handles);
        Print(L"No network interfaces found\n");
        return;
    }

    Print(L"NIC  MAC                Link  IP\n");
    Print(L"---  -----------------  ----  ---------------\n");

    for (UINTN i = 0; i < HandleCount; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *Snp = NULL;
        Status = gBS->OpenProtocol(
            Handles[i], &gEfiSimpleNetworkProtocolGuid, (VOID **)&Snp,
            ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(Status) || Snp == NULL) continue;

        // MAC address
        EFI_MAC_ADDRESS *Mac = &Snp->Mode->CurrentAddress;
        UINT32 MacLen = Snp->Mode->HwAddressSize;
        if (MacLen > 6) MacLen = 6;

        // Link status
        CHAR16 *LinkStr = L"??";
        if (Snp->Mode->MediaPresentSupported) {
            LinkStr = Snp->Mode->MediaPresent ? L"UP" : L"DOWN";
        }

        // Try to get current IP via IP4Config2
        CHAR16 IpStr[16];
        StrCpyS(IpStr, sizeof(IpStr) / sizeof(CHAR16), L"(none)");

        EFI_IP4_CONFIG2_PROTOCOL *Ip4Cfg2 = NULL;
        Status = gBS->OpenProtocol(
            Handles[i], &gEfiIp4Config2ProtocolGuid, (VOID **)&Ip4Cfg2,
            ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (!EFI_ERROR(Status) && Ip4Cfg2 != NULL) {
            UINTN DataSize = 0;
            Status = Ip4Cfg2->GetData(
                Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, NULL);
            if (Status == EFI_BUFFER_TOO_SMALL && DataSize > 0) {
                EFI_IP4_CONFIG2_INTERFACE_INFO *IfInfo = AllocatePool(DataSize);
                if (IfInfo != NULL) {
                    Status = Ip4Cfg2->GetData(
                        Ip4Cfg2, Ip4Config2DataTypeInterfaceInfo, &DataSize, IfInfo);
                    if (!EFI_ERROR(Status) &&
                        (IfInfo->StationAddress.Addr[0] | IfInfo->StationAddress.Addr[1] |
                         IfInfo->StationAddress.Addr[2] | IfInfo->StationAddress.Addr[3]) != 0) {
                        UnicodeSPrint(IpStr, sizeof(IpStr), L"%d.%d.%d.%d",
                            IfInfo->StationAddress.Addr[0], IfInfo->StationAddress.Addr[1],
                            IfInfo->StationAddress.Addr[2], IfInfo->StationAddress.Addr[3]);
                    }
                    FreePool(IfInfo);
                }
            }
        }

        Print(L"%3d  %02x:%02x:%02x:%02x:%02x:%02x  %-4s  %s\n",
              i,
              Mac->Addr[0], Mac->Addr[1], Mac->Addr[2],
              Mac->Addr[3], Mac->Addr[4], Mac->Addr[5],
              LinkStr, IpStr);
    }

    FreePool(Handles);
}
