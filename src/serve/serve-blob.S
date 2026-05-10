/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* serve-blob.S -- embeds axl-webfs-serve-dxe.efi into axl-webfs.efi
 * via .incbin. Mirrors the mkrd-blob.S / service-demo-launch-blob.S
 * pattern: the build system supplies the per-arch blob path via
 * -DBLOB_PATH=... and this .S stays arch-agnostic. */

#ifndef BLOB_PATH
#  error "BLOB_PATH must be defined (see Makefile)"
#endif

#define _AXL_STR(x) #x
#define _AXL_XSTR(x) _AXL_STR(x)

    .section .rodata
    .balign 8
    .globl axl_embedded_axl_webfs_serve_dxe
    .globl axl_embedded_axl_webfs_serve_dxe_end
axl_embedded_axl_webfs_serve_dxe:
    .incbin _AXL_XSTR(BLOB_PATH)
axl_embedded_axl_webfs_serve_dxe_end:

#if defined(__ELF__) && defined(__linux__)
    .section .note.GNU-stack, "", %progbits
#endif
