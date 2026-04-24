# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0

AXL_SDK  ?= /usr
AXL_CC   = $(AXL_SDK)/bin/axl-cc
ARCH     ?= x64
OUTDIR   = build/axl/$(ARCH)

APP_SRCS = src/app/main.c \
           src/app/cmd-serve.c \
           src/app/cmd-mount.c \
           src/net/network.c \
           src/transfer/file-transfer.c \
           src/transfer/dir-list.c

DRV_SRCS = src/driver/webfs.c \
           src/driver/webfs-file.c \
           src/driver/webfs-cache.c \
           src/net/network.c

CFLAGS   = -Isrc

all: axl-webfs axl-webfs-dxe

axl-webfs: $(OUTDIR)/axl-webfs.efi
$(OUTDIR)/axl-webfs.efi: $(APP_SRCS) | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) $(CFLAGS) $(APP_SRCS) -o $@

axl-webfs-dxe: $(OUTDIR)/axl-webfs-dxe.efi
$(OUTDIR)/axl-webfs-dxe.efi: $(DRV_SRCS) | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) $(DRV_SRCS) -o $@

$(OUTDIR):
	mkdir -p $@

clean:
	rm -rf build/axl
