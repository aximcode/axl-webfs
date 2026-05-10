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

# axl-webfs.efi sources. The serve driver image is spliced into
# .rodata via `axl-cc --embed` (see the link rule below) so
# `serve --detach` can load it without a sidecar file; the build
# rule has a build-time dep on the driver image.
APP_SRCS = src/app/main.c \
           src/app/cmd-serve.c \
           src/app/cmd-mount.c \
           src/net/network.c \
           src/serve/serve-core.c \
           src/serve/upload-asset.c \
           src/transfer/file-transfer.c \
           src/transfer/dir-list.c

DRV_SRCS = src/driver/webfs.c \
           src/driver/webfs-file.c \
           src/driver/webfs-cache.c \
           src/net/network.c

SERVE_DRV_SRCS = src/driver/serve-dxe.c \
                 src/net/network.c \
                 src/serve/serve-core.c \
                 src/serve/upload-asset.c \
                 src/transfer/file-transfer.c \
                 src/transfer/dir-list.c

CFLAGS   = -Isrc

all: axl-webfs axl-webfs-dxe axl-webfs-serve-dxe

# axl-webfs.efi embeds both DXE drivers via axl-cc --embed (which
# generates a .incbin sidecar): the serve driver for `serve --detach`
# (loaded via axl_service_launch_embedded), and the mount driver for
# `mount` (loaded via axl_driver_load_buffer). Single-binary toolkit;
# the standalone .efi files below are kept for users who prefer the
# UEFI-shell `load` workflow.
axl-webfs: $(OUTDIR)/axl-webfs.efi
$(OUTDIR)/axl-webfs.efi: $(APP_SRCS) \
                         $(OUTDIR)/axl-webfs-serve-dxe.efi \
                         $(OUTDIR)/axl-webfs-dxe.efi | $(OUTDIR) $(SDK_SYNC_DEP)
	$(AXL_CC) --arch $(ARCH) $(CFLAGS) \
	    --embed $(OUTDIR)/axl-webfs-serve-dxe.efi=axl_webfs_serve_dxe \
	    --embed $(OUTDIR)/axl-webfs-dxe.efi=axl_webfs_mount_dxe \
	    $(APP_SRCS) -o $@

axl-webfs-dxe: $(OUTDIR)/axl-webfs-dxe.efi
$(OUTDIR)/axl-webfs-dxe.efi: $(DRV_SRCS) | $(OUTDIR) $(SDK_SYNC_DEP)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) $(DRV_SRCS) -o $@

axl-webfs-serve-dxe: $(OUTDIR)/axl-webfs-serve-dxe.efi
$(OUTDIR)/axl-webfs-serve-dxe.efi: $(SERVE_DRV_SRCS) | $(OUTDIR) $(SDK_SYNC_DEP)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) $(SERVE_DRV_SRCS) -o $@

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

.PHONY: all axl-webfs axl-webfs-dxe axl-webfs-serve-dxe clean demo demo-mount demo-serve
