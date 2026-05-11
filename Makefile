# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0

ARCH     ?= x64
OUTDIR   = build/axl/$(ARCH)

# Pin the default goal explicitly so the conditional sdk-sync rule
# below can't accidentally become the default by virtue of being the
# first rule in the file.
.DEFAULT_GOAL := all

# When AXL_SDK_SRC points at a checkout, treat axl-webfs as a
# first-class consumer of the SDK source: derive AXL_SDK from it
# and (re)build / install the SDK before linking. install.sh's make
# is incremental, so rebuilds when the user pulls SDK changes and
# is a near-no-op otherwise. Without AXL_SDK_SRC the build behaves
# as a packaged-install consumer and AXL_SDK defaults to /usr.
ifneq ($(AXL_SDK_SRC),)
override AXL_SDK := $(AXL_SDK_SRC)/out
.PHONY: sdk-sync
sdk-sync:
	@$(AXL_SDK_SRC)/scripts/install.sh --arch $(ARCH) >/dev/null
SDK_SYNC_DEP := sdk-sync
else
AXL_SDK ?= /usr
SDK_SYNC_DEP :=
endif

AXL_CC   = $(AXL_SDK)/bin/axl-cc

# Launcher (axl-webfs.efi) sources. webfs-serve.c is dual-compiled:
# without -DAXL_SERVICE_BUILD_DRIVER it emits only the descriptor +
# opts_descs that axl_service_start_embedded reads to serialize
# LoadOptions, so the launcher doesn't drag in serve-core impl,
# upload-asset, file-transfer, or dir-list.
APP_SRCS = src/app/main.c \
           src/app/cmd-serve.c \
           src/app/cmd-mount.c \
           src/serve/webfs-serve.c \
           src/mount/webfs-mount.c

# Mount driver image. -DAXL_SERVICE_BUILD_DRIVER pulls in setup,
# teardown, EFI_FILE_PROTOCOL impl, and AXL_SERVICE_DRIVER entry
# from webfs-mount.c. webfs-file.c and webfs-cache.c are the
# protocol-callback / cache supporting files (driver-only).
MOUNT_DRV_SRCS = src/mount/webfs-mount.c \
                 src/mount/webfs-file.c \
                 src/mount/webfs-cache.c \
                 src/mount/webfs-protocol-json.c \
                 src/mount/webfs-protocol-dav.c

# Serve driver image. -DAXL_SERVICE_BUILD_DRIVER pulls in setup,
# teardown, route handlers, and the AXL_SERVICE_DRIVER entry point
# from webfs-serve.c.
SERVE_DRV_SRCS = src/serve/webfs-serve.c \
                 src/serve/webfs-dav.c \
                 src/serve/upload-asset.c \
                 src/transfer/file-transfer.c \
                 src/transfer/dir-list.c

CFLAGS   = -Isrc

all: axl-webfs axl-webfs-mount-dxe axl-webfs-serve-dxe

# axl-webfs.efi embeds both AxlService driver images via axl-cc
# --embed (which generates a .incbin sidecar). serve and mount each
# go through axl_service_start_embedded; the launcher decodes which
# verb the user invoked, populates the matching opts struct, and
# calls start_embedded against the matching deploy descriptor.
# Single-binary toolkit; the standalone -dxe.efi files below are
# also emitted for users who prefer the UEFI-shell `load` workflow.
axl-webfs: $(OUTDIR)/axl-webfs.efi
$(OUTDIR)/axl-webfs.efi: $(APP_SRCS) \
                         $(OUTDIR)/axl-webfs-serve-dxe.efi \
                         $(OUTDIR)/axl-webfs-mount-dxe.efi | $(OUTDIR) $(SDK_SYNC_DEP)
	$(AXL_CC) --arch $(ARCH) $(CFLAGS) \
	    --embed $(OUTDIR)/axl-webfs-serve-dxe.efi=axl_webfs_serve_dxe \
	    --embed $(OUTDIR)/axl-webfs-mount-dxe.efi=axl_webfs_mount_dxe \
	    $(APP_SRCS) -o $@

axl-webfs-mount-dxe: $(OUTDIR)/axl-webfs-mount-dxe.efi
$(OUTDIR)/axl-webfs-mount-dxe.efi: $(MOUNT_DRV_SRCS) | $(OUTDIR) $(SDK_SYNC_DEP)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) \
	    -DAXL_SERVICE_BUILD_DRIVER \
	    $(MOUNT_DRV_SRCS) -o $@

axl-webfs-serve-dxe: $(OUTDIR)/axl-webfs-serve-dxe.efi
$(OUTDIR)/axl-webfs-serve-dxe.efi: $(SERVE_DRV_SRCS) | $(OUTDIR) $(SDK_SYNC_DEP)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) \
	    -DAXL_SERVICE_BUILD_DRIVER \
	    $(SERVE_DRV_SRCS) -o $@

$(OUTDIR):
	mkdir -p $@

clean:
	rm -rf build/axl

# Regenerate the README demo GIFs.
#  demo-mount: vhs + tmux narrative of the mount command.
#  demo-serve: real axl-webfs.efi serve in QEMU, screenshot with
#              headless Chrome. Requires AXL_SDK_SRC for run-qemu.sh.
demo: demo-mount demo-serve
demo-mount:
	vhs docs/assets/demo-mount.tape
demo-serve:
	scripts/demo-serve.sh

.PHONY: all axl-webfs axl-webfs-mount-dxe axl-webfs-serve-dxe clean demo demo-mount demo-serve
