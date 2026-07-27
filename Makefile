# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0

ARCH     ?= x64
OUTDIR   = build/axl/$(ARCH)

# Build the SDK (and link) with TLS so `serve --tls` works. Always on by
# default -- the ~200 KB lands in the serve driver only. Override with
# `make AXL_TLS=` to drop it (TLS functions then return failure and
# `--tls` errors at runtime).
AXL_TLS  ?= 1

# Pin the default goal explicitly rather than relying on `all` happening to
# be the first rule in the file. Also read by LINK_GOALS below, which needs
# the effective goal when MAKECMDGOALS is empty.
.DEFAULT_GOAL := all

# Goals that actually link something. Gates both the SDK sync and the
# link-input stamp below, so `make clean` neither builds an SDK nor
# recreates the output directory it just deleted.
LINK_GOALS := $(filter-out clean help demo demo-mount demo-serve,\
                $(or $(MAKECMDGOALS),$(.DEFAULT_GOAL)))

# Both of those run as $(shell ...) during parse (see why below), and a
# $(shell) in a variable assignment executes even under `make -n` / -t / -q
# -- those flags suppress RECIPE execution, not makefile evaluation. Without
# this guard a dry run would really build and install an SDK and really
# write the stamp. Detect it from the short-flag cluster in MAKEFLAGS.
DRY_RUN    := $(findstring n,$(firstword -$(MAKEFLAGS)))
DO_SDK_SYNC := $(if $(DRY_RUN),,$(LINK_GOALS))

# When AXL_SDK_SRC points at a checkout, treat axl-webfs as a
# first-class consumer of the SDK source: derive AXL_SDK from it
# and (re)build / install the SDK before linking. install.sh's make
# is incremental, so rebuilds when the user pulls SDK changes and
# is a near-no-op otherwise. Without AXL_SDK_SRC the build behaves
# as a packaged-install consumer and AXL_SDK defaults to /usr.
ifneq ($(AXL_SDK_SRC),)
override AXL_SDK := $(AXL_SDK_SRC)/out

# The sync runs at PARSE time, deliberately, not as a recipe prerequisite.
#
# It used to be an order-only prerequisite (`| $(OUTDIR) sdk-sync`). That
# sequenced the SDK rebuild ahead of the link correctly, but make had
# already stat'd libaxl.a by the time install.sh rewrote it -- so an
# SDK-only fix left the previous .efi in place and the next test run
# silently exercised a STALE binary against a fresh SDK. Adding libaxl.a as
# a normal prerequisite (AXL_LIB_STAMP below) is necessary but NOT
# sufficient on its own: it only takes effect on the *next* make. Syncing
# during parse means the installed libs carry their final mtimes before
# make evaluates a single dependency, so one `make` is enough.
#
# Skipped for goals that never link, so `make clean` doesn't build an SDK.
ifneq ($(DO_SDK_SYNC),)
SDK_SYNC_ERR := $(shell $(if $(AXL_TLS),AXL_TLS=$(AXL_TLS)) \
    $(AXL_SDK_SRC)/scripts/install.sh --arch $(ARCH) >/dev/null || echo FAILED)
ifeq ($(SDK_SYNC_ERR),FAILED)
$(error AXL SDK sync failed -- rerun by hand to see why: \
  $(AXL_SDK_SRC)/scripts/install.sh --arch $(ARCH))
endif
endif
else
AXL_SDK ?= /usr
endif

AXL_CC   = $(AXL_SDK)/bin/axl-cc

# Every .efi links the SDK's static lib plus the CRT0 / reloc / debug-info
# objects, so an SDK-ONLY change -- a fix inside libaxl.a with no axl-webfs
# source touched -- has to force a relink. Each .efi therefore carries
# AXL_LIB_STAMP as a NORMAL prerequisite, BEFORE the `|` in its rule:
# anything after the `|` is order-only, which sequences work but can never
# mark a target out of date. That is exactly how a fresh libaxl.a used to
# leave a stale .efi in place and cost an hour of false negatives.
#
# The dependency is on a CONTENT stamp rather than on the libs' mtimes,
# because install.sh reinstalls (and so re-dates) libaxl.a on every run even
# when it rebuilt nothing -- a bare mtime dependency would relink all three
# .efi files on every single make. Hash the real link inputs after the sync
# and touch the stamp only when that hash moves, so a relink means the SDK
# actually changed.
#
# Both this and the sync run at parse time so the stamp is settled before
# make evaluates any dependency. `cat` of a missing lib is tolerated (empty
# hash), so a first-ever build against an unbuilt SDK still works.
AXL_LIBDIR    := $(AXL_SDK)/lib/axl/$(ARCH)
AXL_LIB_STAMP := $(OUTDIR)/.axl-sdk-lib.stamp

# EVERYTHING axl-cc feeds the link, because missing one input reintroduces
# exactly the staleness this machinery exists to prevent. Note the linker
# script and version script live one level ABOVE the per-arch dir -- axl-cc
# passes `-T $(AXL_SDK)/lib/axl/elf_<arch>_efi.lds` and `--version-script
# $(AXL_SDK)/lib/axl/efi-localize.ver` -- so hashing only $(AXL_LIBDIR)
# would miss a linker-script-only SDK change and silently keep a stale .efi.
# axl-cc itself is in here too: it chooses the link flags, so a change to it
# changes the output. Both arches' .lds are globbed rather than mapped by
# name (x64 -> elf_x86_64); over-hashing costs at most an extra relink,
# under-hashing costs a stale binary.
AXL_LINK_INPUTS := $(AXL_LIBDIR)/libaxl.a $(AXL_LIBDIR)/*.o \
                   $(AXL_SDK)/lib/axl/*.lds $(AXL_SDK)/lib/axl/*.ver \
                   $(AXL_SDK)/bin/axl-cc

ifneq ($(DO_SDK_SYNC),)
$(shell mkdir -p $(OUTDIR); \
        new=$$(cat $(AXL_LINK_INPUTS) 2>/dev/null \
               | sha256sum | cut -d' ' -f1); \
        old=$$(cat $(AXL_LIB_STAMP).hash 2>/dev/null); \
        if [ "$$new" != "$$old" ]; then \
            printf '%s' "$$new" > $(AXL_LIB_STAMP).hash; \
            touch $(AXL_LIB_STAMP); \
        fi)
endif

# Fallback so the stamp is never a missing prerequisite (e.g. building an
# explicit .efi path, or a hand-deleted stamp).
$(AXL_LIB_STAMP):
	@mkdir -p $(@D) && touch $@

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
$(OUTDIR)/axl-webfs.efi: $(APP_SRCS) $(AXL_LIB_STAMP) \
                         $(OUTDIR)/axl-webfs-serve-dxe.efi \
                         $(OUTDIR)/axl-webfs-mount-dxe.efi | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) $(CFLAGS) \
	    --embed $(OUTDIR)/axl-webfs-serve-dxe.efi=axl_webfs_serve_dxe \
	    --embed $(OUTDIR)/axl-webfs-mount-dxe.efi=axl_webfs_mount_dxe \
	    $(APP_SRCS) -o $@

axl-webfs-mount-dxe: $(OUTDIR)/axl-webfs-mount-dxe.efi
$(OUTDIR)/axl-webfs-mount-dxe.efi: $(MOUNT_DRV_SRCS) $(AXL_LIB_STAMP) | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) \
	    -DAXL_SERVICE_BUILD_DRIVER \
	    $(MOUNT_DRV_SRCS) -o $@

axl-webfs-serve-dxe: $(OUTDIR)/axl-webfs-serve-dxe.efi
$(OUTDIR)/axl-webfs-serve-dxe.efi: $(SERVE_DRV_SRCS) $(AXL_LIB_STAMP) | $(OUTDIR)
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
