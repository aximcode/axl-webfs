/** @file
  HttpFS -- AXL File Transfer Toolkit entry point.

  Parses command line parameters and dispatches to the
  appropriate command handler: serve, mount, or umount.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "HttpFsInternal.h"

#include <axl.h>
#include <axl/axl-net.h>

#define HTTPFS_VERSION  "0.1"

static void PrintUsage(void)
{
    axl_printf("HttpFS v%s -- File Transfer Toolkit\n\n", HTTPFS_VERSION);
    axl_printf("Usage:\n");
    axl_printf("  HttpFS mount <url> [-r]   Mount remote directory as volume\n");
    axl_printf("  HttpFS umount             Unmount remote volume\n");
    axl_printf("  HttpFS serve [options]    Run HTTP file server\n");
    axl_printf("  HttpFS list-nics          List network interfaces\n");
    axl_printf("  HttpFS -h                 Show this help\n");
    axl_printf("\nExamples:\n");
    axl_printf("  HttpFS mount http://10.0.0.5:8080/\n");
    axl_printf("  HttpFS umount\n");
}

static int CmdListNics(void)
{
    size_t count = 0;

    axl_net_list_interfaces(NULL, &count);
    if (count == 0) {
        axl_printf("No network interfaces found\n");
        return 0;
    }

    AxlNetInterface *nics = axl_malloc(count * sizeof(AxlNetInterface));
    if (nics == NULL) {
        axl_printf("ERROR: Failed to allocate memory for NIC list\n");
        return 1;
    }

    axl_net_list_interfaces(nics, &count);

    axl_printf("NIC  MAC                Link  IP\n");
    axl_printf("---  -----------------  ----  ---------------\n");
    for (size_t i = 0; i < count; i++) {
        char ip_str[16] = "(none)";
        if (nics[i].has_ipv4) {
            axl_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                nics[i].ipv4[0], nics[i].ipv4[1],
                nics[i].ipv4[2], nics[i].ipv4[3]);
        }
        axl_printf("%3zu  %02x:%02x:%02x:%02x:%02x:%02x  %-4s  %s\n",
            i,
            nics[i].mac[0], nics[i].mac[1], nics[i].mac[2],
            nics[i].mac[3], nics[i].mac[4], nics[i].mac[5],
            nics[i].link_up ? "UP" : "DOWN",
            nics[i].has_ipv4 ? ip_str : "(none)");
    }

    axl_free(nics);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    const char *cmd = argv[1];

    if (axl_streql(cmd, "-h") || axl_streql(cmd, "--help") ||
        axl_streql(cmd, "help")) {
        PrintUsage();
        return 0;
    }

    if (axl_streql(cmd, "mount")) {
        return CmdMount(argc - 1, argv + 1);
    }

    if (axl_streql(cmd, "umount")) {
        return CmdUmount(argc - 1, argv + 1);
    }

    if (axl_streql(cmd, "serve")) {
        return CmdServe(argc - 1, argv + 1);
    }

    if (axl_streql(cmd, "list-nics")) {
        return CmdListNics();
    }

    axl_printf("ERROR: Unknown command '%s'\n\n", cmd);
    PrintUsage();
    return 1;
}
