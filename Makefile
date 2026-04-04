AXL_SDK  ?= $(HOME)/projects/aximcode/axl-sdk/out
AXL_CC   = $(AXL_SDK)/bin/axl-cc
ARCH     ?= x64
OUTDIR   = build/axl/$(ARCH)

APP_SRCS = src/app/main.c \
           src/app/cmd-serve.c \
           src/app/cmd-mount.c \
           src/net/network.c \
           src/transfer/file-transfer.c \
           src/transfer/dir-list.c

DRV_SRCS = src/driver/webdavfs.c \
           src/driver/webdavfs-file.c \
           src/driver/webdavfs-cache.c \
           src/net/network.c

CFLAGS   = -Isrc

all: HttpFS WebDavFsDxe

HttpFS: $(OUTDIR)/HttpFS.efi
$(OUTDIR)/HttpFS.efi: $(APP_SRCS) | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) $(CFLAGS) $(APP_SRCS) -o $@

WebDavFsDxe: $(OUTDIR)/WebDavFsDxe.efi
$(OUTDIR)/WebDavFsDxe.efi: $(DRV_SRCS) | $(OUTDIR)
	$(AXL_CC) --arch $(ARCH) --type driver $(CFLAGS) $(DRV_SRCS) -o $@

$(OUTDIR):
	mkdir -p $@

clean:
	rm -rf build/axl
