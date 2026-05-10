# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0

AXL_SDK  ?= /usr
AXL_CC   = $(AXL_SDK)/bin/axl-cc
ARCH     ?= x64
OUTDIR   = build/axl/$(ARCH)

# axl-webfs.efi sources. Includes serve-blob.S, which uses .incbin
# to splice the prebuilt axl-webfs-serve-dxe.efi into .rodata so
# `serve --detach` can load it without a sidecar file. The .efi
# build below has a build-time dep on the driver image.
APP_SRCS = src/app/main.c \
           src/app/cmd-serve.c \
           src/app/cmd-mount.c \
           src/net/network.c \
           src/serve/serve-blob.S \
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

# axl-webfs.efi depends on the serve driver image because serve-blob.S
# .incbin's it; refreshed driver bytes propagate into the launcher on
# the next link.
axl-webfs: $(OUTDIR)/axl-webfs.efi
$(OUTDIR)/axl-webfs.efi: $(APP_SRCS) $(OUTDIR)/axl-webfs-serve-dxe.efi | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) $(CFLAGS) \
	    -DBLOB_PATH=$(OUTDIR)/axl-webfs-serve-dxe.efi \
	    $(APP_SRCS) -o $@

axl-webfs-dxe: $(OUTDIR)/axl-webfs-dxe.efi
$(OUTDIR)/axl-webfs-dxe.efi: $(DRV_SRCS) | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) $(DRV_SRCS) -o $@

axl-webfs-serve-dxe: $(OUTDIR)/axl-webfs-serve-dxe.efi
$(OUTDIR)/axl-webfs-serve-dxe.efi: $(SERVE_DRV_SRCS) | $(OUTDIR)
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
