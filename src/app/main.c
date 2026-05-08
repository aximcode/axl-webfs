/** @file
  axl-webfs -- AXL File Transfer Toolkit entry point.

  Builds a declarative AxlArgsNode verb tree (serve, mount, umount,
  list-nics) and hands it to axl_args_run, which parses and dispatches.
  Verb handlers and their flag/positional tables live in cmd-serve.c
  and cmd-mount.c; list-nics is small and stays inline here.

  Copyright (c) 2026, AximCode. All rights reserved.
  SPDX-License-Identifier: Apache-2.0
**/

#include "webfs-internal.h"

#include <axl.h>
#include <axl/axl-args.h>
#include <axl/axl-net.h>

/* Verb handlers and flag tables defined in cmd-serve.c / cmd-mount.c. */
extern const AxlArgDesc webfs_serve_flags[];
extern const AxlArgDesc webfs_mount_flags[];
extern const AxlArgDesc webfs_mount_pos[];
extern int webfs_serve_handler(AxlArgs *a);
extern int webfs_mount_handler(AxlArgs *a);
extern int webfs_umount_handler(AxlArgs *a);

static int
webfs_list_nics_handler(AxlArgs *a)
{
    (void)a;

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

static const AxlArgsNode verbs[] = {
    { .name        = "serve",
      .help        = "Run HTTP file server",
      .flags       = webfs_serve_flags,
      .handler     = webfs_serve_handler },
    { .name        = "mount",
      .help        = "Mount a remote axl-webfs server URL as a UEFI volume",
      .flags       = webfs_mount_flags,
      .positionals = webfs_mount_pos,
      .handler     = webfs_mount_handler },
    { .name        = "umount",
      .help        = "Unmount the previously mounted axl-webfs volume",
      .handler     = webfs_umount_handler },
    { .name        = "list-nics",
      .help        = "List network interfaces",
      .handler     = webfs_list_nics_handler },
    {0},
};

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "axl-webfs",
        .help        = "UEFI File Transfer Toolkit (v" AXL_WEBFS_VERSION ")",
        .help_prolog = "Bidirectional file transfer and remote filesystem "
                       "access for UEFI hosts. Use `serve` to expose local "
                       "volumes over HTTP, or `mount` to attach a remote "
                       "axl-webfs server as a UEFI volume.",
        .help_epilog = "Examples:\n"
                       "  axl-webfs serve -p 8080\n"
                       "  axl-webfs mount http://10.0.0.5:8080/\n"
                       "  axl-webfs umount\n"
                       "  axl-webfs list-nics",
        .verbs       = verbs,
    });
}
