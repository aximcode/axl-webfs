/** @file
  NetworkLib -- NIC discovery, DHCP configuration, and static IP setup.

  Adapted from SoftBMC Core/Network.c. Auto-loads NIC drivers from
  \drivers\{arch}\ on mounted filesystems, connects network stack
  drivers, and auto-selects the first NIC with an IP. Single-NIC init
  with IP4Config2 DHCP + DHCP4 direct fallback.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/NetworkLib.h>

#include <axl.h>
#include <axl/axl-driver.h>
#include <axl/axl-sys.h>
#include <axl/axl-net.h>
#include <uefi/axl-uefi.h>



/* ------------------------------------------------------------------ */
/* IP4Config2 types not in the generated UEFI headers                 */
/* ------------------------------------------------------------------ */

typedef enum {
    Ip4Config2PolicyStatic = 0,
    Ip4Config2PolicyDhcp   = 1
} EFI_IP4_CONFIG2_POLICY;

typedef struct {
    EFI_IPv4_ADDRESS  Address;
    EFI_IPv4_ADDRESS  SubnetMask;
} EFI_IP4_CONFIG2_MANUAL_ADDRESS;

/* ------------------------------------------------------------------ */
/* Architecture-dependent driver path                                 */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(_M_X64)
  #define DRIVER_DIR_PATH  "\\drivers\\x64"
#elif defined(__aarch64__)
  #define DRIVER_DIR_PATH  "\\drivers\\aa64"
#else
  #define DRIVER_DIR_PATH  "\\drivers"
#endif

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static bool         mInitialized = false;
static NetworkInfo  mIfaceInfo;
static void        *mNicHandle = NULL;  /* IP4Config2 handle */

/* DHCP4 direct fallback state */
static bool   mDhcp4Fallback = false;
static void  *mDhcp4SbHandle = NULL;
static void  *mDhcp4ChildHandle = NULL;

/* ------------------------------------------------------------------ */
/* IP4Config2 helpers                                                 */
/* ------------------------------------------------------------------ */

/** Check if a handle already has a non-zero IP via IP4Config2. */
static int
check_existing_ip(void *handle, NetworkInfo *info)
{
    EFI_IP4_CONFIG2_PROTOCOL *ip4cfg2 = NULL;
    if (axl_handle_get_service(handle, "ip4-config2", (void **)&ip4cfg2) != 0)
        return -1;

    UINTN data_size = 0;
    EFI_STATUS status = ip4cfg2->GetData(
        ip4cfg2, Ip4Config2DataTypeInterfaceInfo, &data_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL || data_size == 0)
        return -1;

    EFI_IP4_CONFIG2_INTERFACE_INFO *if_info = axl_malloc(data_size);
    if (if_info == NULL) return -1;

    status = ip4cfg2->GetData(
        ip4cfg2, Ip4Config2DataTypeInterfaceInfo, &data_size, if_info);
    if (EFI_ERROR(status)) { axl_free(if_info); return -1; }

    /* Check for non-zero IP */
    if ((if_info->StationAddress.Addr[0] | if_info->StationAddress.Addr[1] |
         if_info->StationAddress.Addr[2] | if_info->StationAddress.Addr[3]) == 0) {
        axl_free(if_info);
        return -1;
    }

    axl_memset(info, 0, sizeof(*info));
    info->valid = true;
    axl_memcpy(info->ip, &if_info->StationAddress, 4);
    axl_memcpy(info->subnet_mask, &if_info->SubnetMask, 4);
    axl_memcpy(info->mac, &if_info->HwAddress, 6);
    info->mac_len = if_info->HwAddressSize;
    axl_free(if_info);

    /* Query gateway (separate data type) */
    UINTN gw_size = 0;
    status = ip4cfg2->GetData(
        ip4cfg2, Ip4Config2DataTypeGateway, &gw_size, NULL);
    if (status == EFI_BUFFER_TOO_SMALL && gw_size >= 4) {
        EFI_IPv4_ADDRESS *gw = axl_malloc(gw_size);
        if (gw != NULL) {
            status = ip4cfg2->GetData(
                ip4cfg2, Ip4Config2DataTypeGateway, &gw_size, gw);
            if (!EFI_ERROR(status))
                axl_memcpy(info->gateway, gw, 4);
            axl_free(gw);
        }
    }

    return 0;
}

/** Set DHCP policy on a handle and wait for an IP. */
static int
configure_dhcp_via_ip4config2(void *handle, size_t timeout_sec)
{
    EFI_IP4_CONFIG2_PROTOCOL *ip4cfg2 = NULL;
    if (axl_handle_get_service(handle, "ip4-config2", (void **)&ip4cfg2) != 0)
        return -1;

    EFI_IP4_CONFIG2_POLICY dhcp_policy = Ip4Config2PolicyDhcp;
    EFI_STATUS status = ip4cfg2->SetData(
        ip4cfg2, Ip4Config2DataTypePolicy,
        sizeof(EFI_IP4_CONFIG2_POLICY), &dhcp_policy);
    if (EFI_ERROR(status)) return -1;

    /* Poll for IP assignment */
    for (size_t sec = 0; sec < timeout_sec; sec++) {
        axl_stall(1000000);  /* 1 second */

        UINTN data_size = 0;
        status = ip4cfg2->GetData(
            ip4cfg2, Ip4Config2DataTypeInterfaceInfo, &data_size, NULL);
        if (status != EFI_BUFFER_TOO_SMALL || data_size == 0) continue;

        EFI_IP4_CONFIG2_INTERFACE_INFO *info = axl_malloc(data_size);
        if (info == NULL) continue;

        status = ip4cfg2->GetData(
            ip4cfg2, Ip4Config2DataTypeInterfaceInfo, &data_size, info);
        if (!EFI_ERROR(status) &&
            (info->StationAddress.Addr[0] | info->StationAddress.Addr[1] |
             info->StationAddress.Addr[2] | info->StationAddress.Addr[3]) != 0) {
            axl_free(info);
            return 0;
        }
        axl_free(info);
    }

    return -1;  /* timeout */
}

/** Configure a static IP on a handle via IP4Config2. */
static int
configure_static_ip(void *handle, const uint8_t *static_ip)
{
    EFI_IP4_CONFIG2_PROTOCOL *ip4cfg2 = NULL;
    if (axl_handle_get_service(handle, "ip4-config2", (void **)&ip4cfg2) != 0)
        return -1;

    /* Set static policy first */
    EFI_IP4_CONFIG2_POLICY static_policy = Ip4Config2PolicyStatic;
    EFI_STATUS status = ip4cfg2->SetData(
        ip4cfg2, Ip4Config2DataTypePolicy,
        sizeof(EFI_IP4_CONFIG2_POLICY), &static_policy);
    if (EFI_ERROR(status)) return -1;

    /* Set manual address with /24 default subnet */
    EFI_IP4_CONFIG2_MANUAL_ADDRESS manual_addr;
    axl_memset(&manual_addr, 0, sizeof(manual_addr));
    axl_memcpy(&manual_addr.Address, static_ip, 4);
    manual_addr.SubnetMask.Addr[0] = 255;
    manual_addr.SubnetMask.Addr[1] = 255;
    manual_addr.SubnetMask.Addr[2] = 255;
    manual_addr.SubnetMask.Addr[3] = 0;

    status = ip4cfg2->SetData(
        ip4cfg2, Ip4Config2DataTypeManualAddress,
        sizeof(manual_addr), &manual_addr);
    return EFI_ERROR(status) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Link status check                                                  */
/* ------------------------------------------------------------------ */

/** Check link status via SNP. Warns if cable unplugged. */
static void
check_link_status(void *nic_handle)
{
    EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
    if (axl_handle_get_service(nic_handle, "simple-network",
                               (void **)&snp) != 0 || snp == NULL)
        return;

    if (snp->Mode->MediaPresentSupported && !snp->Mode->MediaPresent) {
        axl_printf("WARNING: NIC reports no link (cable unplugged?)\n");
        axl_printf("  Some firmware reports link-down incorrectly -- continuing anyway.\n");
    }
}

/* ------------------------------------------------------------------ */
/* Auto-load NIC drivers from filesystem                              */
/* ------------------------------------------------------------------ */

/** Search for NIC drivers in \drivers\{arch}\ relative to the app's
    location on disk. Uses axl_driver_get_image_path() to find the
    app directory, then scans for .efi files. */
static size_t
load_nic_drivers(void)
{
    size_t loaded = 0;

    /* Get the app's image path, e.g. "\HttpFS.efi" or "\tools\HttpFS.efi" */
    char *img_path = axl_driver_get_image_path();
    if (img_path == NULL)
        return 0;

    /* Derive directory: strip filename to get e.g. "\" or "\tools\" */
    char dir_base[256];
    axl_strlcpy(dir_base, img_path, sizeof(dir_base));
    axl_free(img_path);

    /* Find last separator */
    size_t last_sep = 0;
    for (size_t i = 0; dir_base[i]; i++) {
        if (dir_base[i] == '\\' || dir_base[i] == '/')
            last_sep = i;
    }
    dir_base[last_sep + 1] = '\0';  /* keep trailing separator */

    /* Build driver directory path: <app_dir>\drivers\{arch} */
    char drv_dir[512];
    axl_snprintf(drv_dir, sizeof(drv_dir), "%s%s", dir_base, DRIVER_DIR_PATH);

    AxlDir *dir = axl_dir_open(drv_dir);
    if (dir == NULL) {
        /* No drivers directory — try scanning all mounted volumes */
        void **fs_handles = NULL;
        size_t fs_count = 0;
        if (axl_service_enumerate("simple-fs", &fs_handles, &fs_count) != 0 ||
            fs_count == 0) {
            axl_free(fs_handles);
            return 0;
        }
        for (size_t i = 0; i < fs_count; i++) {
            char *label = axl_volume_get_label_by_handle(fs_handles[i]);
            if (label == NULL) continue;
            axl_snprintf(drv_dir, sizeof(drv_dir), "%s:%s", label, DRIVER_DIR_PATH);
            axl_free(label);
            dir = axl_dir_open(drv_dir);
            if (dir != NULL) break;
        }
        axl_free(fs_handles);
        if (dir == NULL) return 0;
    }

    AxlDirEntry entry;
    while (axl_dir_read(dir, &entry)) {
        if (entry.is_dir) continue;
        size_t name_len = axl_strlen(entry.name);
        if (name_len <= 4) continue;
        if (!axl_streql(&entry.name[name_len - 4], ".efi") &&
            !axl_streql(&entry.name[name_len - 4], ".EFI"))
            continue;

        char full_path[512];
        axl_snprintf(full_path, sizeof(full_path), "%s\\%s", drv_dir, entry.name);

        axl_printf("  Loading %s ... ", entry.name);
        AxlDriverHandle drv;
        if (axl_driver_load(full_path, &drv) == 0) {
            if (axl_driver_start(drv) == 0) {
                axl_driver_connect(drv);
                loaded++;
                axl_printf("OK\n");
            } else {
                axl_printf("start failed\n");
                axl_driver_unload(drv);
            }
        } else {
            axl_printf("load failed\n");
        }
    }
    axl_dir_close(dir);
    return loaded;
}

/* ------------------------------------------------------------------ */
/* DHCP4 direct fallback (when IP4Config2 is not available)           */
/* ------------------------------------------------------------------ */

static int
configure_dhcp4_direct(size_t timeout_sec)
{
    (void)timeout_sec;

    void **handles = NULL;
    size_t handle_count = 0;

    if (axl_service_enumerate("dhcp4-sb", &handles, &handle_count) != 0 ||
        handle_count == 0) {
        axl_free(handles);
        return -1;
    }

    axl_printf("  No IP4Config2 -- trying DHCP4 direct...\n");

    for (size_t i = 0; i < handle_count; i++) {
        EFI_SERVICE_BINDING_PROTOCOL *dhcp4_sb = NULL;
        if (axl_handle_get_service(handles[i], "dhcp4-sb",
                                   (void **)&dhcp4_sb) != 0 || dhcp4_sb == NULL)
            continue;

        EFI_HANDLE child_handle = NULL;
        EFI_STATUS st = dhcp4_sb->CreateChild(dhcp4_sb, &child_handle);
        if (EFI_ERROR(st)) continue;

        EFI_DHCP4_PROTOCOL *dhcp4 = NULL;
        if (axl_handle_get_service(child_handle, "dhcp4",
                                   (void **)&dhcp4) != 0) {
            dhcp4_sb->DestroyChild(dhcp4_sb, child_handle);
            continue;
        }

        UINT32 discover_timeout[] = { 4, 8, 16 };
        UINT32 request_timeout[] = { 4, 8 };
        EFI_DHCP4_CONFIG_DATA cfg;
        axl_memset(&cfg, 0, sizeof(cfg));
        cfg.DiscoverTryCount = 3;
        cfg.DiscoverTimeout = discover_timeout;
        cfg.RequestTryCount = 2;
        cfg.RequestTimeout = request_timeout;

        st = dhcp4->Configure(dhcp4, &cfg);
        if (EFI_ERROR(st)) {
            dhcp4_sb->DestroyChild(dhcp4_sb, child_handle);
            continue;
        }

        /* Check link before DHCP4 discover (warn only) */
        {
            EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
            if (axl_handle_get_service(handles[i], "simple-network",
                                       (void **)&snp) == 0 && snp != NULL &&
                snp->Mode->MediaPresentSupported && !snp->Mode->MediaPresent) {
                axl_printf("  NIC %zu: reports no link (may be incorrect)\n", i);
            }
        }

        axl_printf("  DHCP4 discovering...\n");
        st = dhcp4->Start(dhcp4, NULL);
        if (EFI_ERROR(st)) {
            dhcp4->Configure(dhcp4, NULL);
            dhcp4_sb->DestroyChild(dhcp4_sb, child_handle);
            continue;
        }

        EFI_DHCP4_MODE_DATA mode_data;
        st = dhcp4->GetModeData(dhcp4, &mode_data);
        if (EFI_ERROR(st)) {
            dhcp4->Stop(dhcp4);
            dhcp4->Configure(dhcp4, NULL);
            dhcp4_sb->DestroyChild(dhcp4_sb, child_handle);
            continue;
        }

        /* Store results */
        mDhcp4Fallback = true;
        mDhcp4SbHandle = handles[i];
        mDhcp4ChildHandle = child_handle;

        axl_memset(&mIfaceInfo, 0, sizeof(mIfaceInfo));
        mIfaceInfo.valid = true;
        axl_memcpy(mIfaceInfo.ip, &mode_data.ClientAddress, 4);
        axl_memcpy(mIfaceInfo.subnet_mask, &mode_data.SubnetMask, 4);
        axl_memcpy(mIfaceInfo.gateway, &mode_data.RouterAddress, 4);
        axl_memcpy(mIfaceInfo.mac, &mode_data.ClientMacAddress, 6);
        mIfaceInfo.mac_len = 6;
        mNicHandle = handles[i];

        axl_free(handles);
        return 0;
    }

    axl_free(handles);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Network stack bring-up                                             */
/* ------------------------------------------------------------------ */

/** Try to bring up the network stack: connect SNP handles, optionally
    load drivers from filesystem first. Returns the number of IP4Config2
    handles found after connecting. */
static size_t
bring_up_network_stack(void)
{
    void **snp_handles = NULL;
    size_t snp_count = 0;

    if (axl_service_enumerate("simple-network", &snp_handles, &snp_count) != 0
        || snp_count == 0) {
        /* No SNP at all -- try loading NIC drivers from filesystem */
        axl_free(snp_handles);
        size_t loaded = load_nic_drivers();
        if (loaded > 0) {
            if (axl_service_enumerate("simple-network", &snp_handles, &snp_count) != 0
                || snp_count == 0) {
                axl_free(snp_handles);
                return 0;
            }
        } else {
            return 0;
        }
    }

    /* Recursively connect all SNP handles to bring up MNP/ARP/IP4/TCP4 */
    axl_printf("  Connecting network stack on %zu NIC(s)...\n", snp_count);
    for (size_t i = 0; i < snp_count; i++) {
        axl_driver_connect_handle(snp_handles[i]);
    }
    axl_free(snp_handles);

    /* Count IP4Config2 handles now available */
    void **ip4_handles = NULL;
    size_t ip4_count = 0;
    if (axl_service_enumerate("ip4-config2", &ip4_handles, &ip4_count) != 0)
        ip4_count = 0;
    axl_free(ip4_handles);
    return ip4_count;
}

/** Find the IP4Config2 handle that corresponds to a given SNP handle.
    SNP and IP4Config2 may be on the same handle or on parent/child handles
    sharing the same MAC address. */
static void *
find_ip4_handle_for_snp(void *snp_handle)
{
    /* Fast path: IP4Config2 is on the same handle as SNP */
    EFI_IP4_CONFIG2_PROTOCOL *test = NULL;
    if (axl_handle_get_service(snp_handle, "ip4-config2", (void **)&test) == 0)
        return snp_handle;

    /* Slow path: match by MAC address */
    EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
    if (axl_handle_get_service(snp_handle, "simple-network",
                               (void **)&snp) != 0 || snp == NULL)
        return NULL;

    void **ip4_handles = NULL;
    size_t ip4_count = 0;
    if (axl_service_enumerate("ip4-config2", &ip4_handles, &ip4_count) != 0
        || ip4_count == 0) {
        axl_free(ip4_handles);
        return NULL;
    }

    void *match = NULL;
    for (size_t i = 0; i < ip4_count; i++) {
        EFI_IP4_CONFIG2_PROTOCOL *ip4cfg2 = NULL;
        if (axl_handle_get_service(ip4_handles[i], "ip4-config2",
                                   (void **)&ip4cfg2) != 0)
            continue;

        UINTN data_size = 0;
        EFI_STATUS st = ip4cfg2->GetData(
            ip4cfg2, Ip4Config2DataTypeInterfaceInfo, &data_size, NULL);
        if (st != EFI_BUFFER_TOO_SMALL || data_size == 0) continue;

        EFI_IP4_CONFIG2_INTERFACE_INFO *if_info = axl_malloc(data_size);
        if (if_info == NULL) continue;

        st = ip4cfg2->GetData(
            ip4cfg2, Ip4Config2DataTypeInterfaceInfo, &data_size, if_info);
        if (!EFI_ERROR(st)) {
            if (axl_memcmp(&if_info->HwAddress, &snp->Mode->CurrentAddress,
                           snp->Mode->HwAddressSize) == 0) {
                match = ip4_handles[i];
                axl_free(if_info);
                break;
            }
        }
        axl_free(if_info);
    }

    axl_free(ip4_handles);
    return match;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int
network_init(size_t nic_index, const uint8_t *static_ip, size_t timeout_sec)
{
    if (mInitialized) return -1;

    axl_memset(&mIfaceInfo, 0, sizeof(mIfaceInfo));

    /*
     * Phase 1: Ensure IP4Config2 handles exist.
     * Try direct enumeration first, then connect SNP, then load drivers.
     */
    void **ip4_handles = NULL;
    size_t ip4_count = 0;
    int rc = axl_service_enumerate("ip4-config2", &ip4_handles, &ip4_count);

    if (rc != 0 || ip4_count == 0) {
        axl_free(ip4_handles);
        ip4_handles = NULL;
        ip4_count = 0;

        size_t new_count = bring_up_network_stack();
        if (new_count == 0) {
            /* Last resort: DHCP4 direct fallback */
            if (configure_dhcp4_direct(timeout_sec) != 0) {
                axl_printf("ERROR: No network interface found\n");
                return -1;
            }
            mInitialized = true;
            return 0;
        }

        /* Re-enumerate after bringing up the stack */
        rc = axl_service_enumerate("ip4-config2", &ip4_handles, &ip4_count);
        if (rc != 0 || ip4_count == 0) {
            axl_free(ip4_handles);
            if (configure_dhcp4_direct(timeout_sec) != 0) {
                axl_printf("ERROR: No network interface found\n");
                return -1;
            }
            mInitialized = true;
            return 0;
        }
    }

    /*
     * Phase 2: Select a NIC.
     * nic_index uses SNP handle ordering (consistent with axl_net_list_interfaces).
     * Map it to the corresponding IP4Config2 handle.
     */
    void *selected_ip4_handle = NULL;

    if (nic_index != (size_t)-1) {
        /* User specified -n: use SNP-based indexing */
        void **snp_handles = NULL;
        size_t snp_count = 0;
        rc = axl_service_enumerate("simple-network", &snp_handles, &snp_count);
        if (rc != 0 || nic_index >= snp_count) {
            axl_printf("ERROR: NIC index %zu out of range (0-%zu)\n",
                       nic_index, (rc != 0 || snp_count == 0) ? (size_t)0 : snp_count - 1);
            axl_free(snp_handles);
            axl_free(ip4_handles);
            return -1;
        }
        selected_ip4_handle = find_ip4_handle_for_snp(snp_handles[nic_index]);
        if (selected_ip4_handle == NULL) {
            axl_printf("ERROR: NIC %zu has no IP4 stack (try 'connect -r' first)\n",
                       nic_index);
            axl_free(snp_handles);
            axl_free(ip4_handles);
            return -1;
        }
        axl_free(snp_handles);
    } else {
        /* Auto-select: find first NIC that already has an IP */
        for (size_t i = 0; i < ip4_count; i++) {
            NetworkInfo tmp_info;
            if (check_existing_ip(ip4_handles[i], &tmp_info) == 0) {
                selected_ip4_handle = ip4_handles[i];
                axl_printf("  Found NIC with IP %d.%d.%d.%d\n",
                           tmp_info.ip[0], tmp_info.ip[1],
                           tmp_info.ip[2], tmp_info.ip[3]);
                break;
            }
        }
        /* If none have an IP yet, use first handle and DHCP */
        if (selected_ip4_handle == NULL) {
            selected_ip4_handle = ip4_handles[0];
        }
    }

    mNicHandle = selected_ip4_handle;
    axl_free(ip4_handles);

    /* Check link (warning only) */
    check_link_status(selected_ip4_handle);

    /* Static IP requested? */
    if (static_ip != NULL) {
        if (configure_static_ip(selected_ip4_handle, static_ip) != 0) {
            axl_printf("ERROR: Failed to set static IP\n");
            return -1;
        }
        axl_stall(500000);
    }

    /* Check if NIC already has an IP */
    if (check_existing_ip(selected_ip4_handle, &mIfaceInfo) == 0) {
        mInitialized = true;
        return 0;
    }

    /* No IP yet -- run DHCP via IP4Config2 */
    axl_printf("  DHCP...\n");
    if (configure_dhcp_via_ip4config2(selected_ip4_handle, timeout_sec) != 0) {
        axl_printf("ERROR: DHCP timeout after %zus\n", timeout_sec);
        return -1;
    }

    /* Re-read the assigned IP */
    if (check_existing_ip(selected_ip4_handle, &mIfaceInfo) != 0) {
        axl_printf("ERROR: DHCP completed but no IP assigned\n");
        return -1;
    }

    mInitialized = true;
    return 0;
}

int
network_get_address(uint8_t ip_out[4])
{
    if (!mInitialized || !mIfaceInfo.valid) return -1;
    axl_memcpy(ip_out, mIfaceInfo.ip, 4);
    return 0;
}

int
network_get_info(NetworkInfo *info)
{
    if (!mInitialized || !mIfaceInfo.valid) return -1;
    axl_memcpy(info, &mIfaceInfo, sizeof(NetworkInfo));
    return 0;
}

void
network_cleanup(void)
{
    if (mDhcp4Fallback && mDhcp4ChildHandle != NULL) {
        EFI_DHCP4_PROTOCOL *dhcp4 = NULL;
        axl_handle_get_service(mDhcp4ChildHandle, "dhcp4", (void **)&dhcp4);
        if (dhcp4 != NULL) {
            dhcp4->Stop(dhcp4);
            dhcp4->Configure(dhcp4, NULL);
        }

        EFI_SERVICE_BINDING_PROTOCOL *dhcp4_sb = NULL;
        axl_handle_get_service(mDhcp4SbHandle, "dhcp4-sb",
                               (void **)&dhcp4_sb);
        if (dhcp4_sb != NULL) {
            dhcp4_sb->DestroyChild(dhcp4_sb, mDhcp4ChildHandle);
        }

        mDhcp4ChildHandle = NULL;
        mDhcp4SbHandle = NULL;
        mDhcp4Fallback = false;
    }

    axl_memset(&mIfaceInfo, 0, sizeof(mIfaceInfo));
    mNicHandle = NULL;
    mInitialized = false;
}
