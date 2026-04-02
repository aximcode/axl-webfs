AXL_SDK  ?= $(HOME)/projects/aximcode/axl-sdk/out
AXL_CC   = $(AXL_SDK)/bin/axl-cc
ARCH     ?= x64
OUTDIR   = build/axl/$(ARCH)

APP_SRCS = HttpFsPkg/Application/HttpFS/Main.c \
           HttpFsPkg/Application/HttpFS/CmdServe.c \
           HttpFsPkg/Application/HttpFS/CmdMount.c \
           HttpFsPkg/Library/NetworkLib/Network.c \
           HttpFsPkg/Library/FileTransferLib/FileTransfer.c \
           HttpFsPkg/Library/FileTransferLib/DirList.c

DRV_SRCS = HttpFsPkg/Driver/WebDavFsDxe/WebDavFs.c \
           HttpFsPkg/Driver/WebDavFsDxe/WebDavFsFile.c \
           HttpFsPkg/Driver/WebDavFsDxe/WebDavFsCache.c \
           HttpFsPkg/Library/NetworkLib/Network.c

CFLAGS   = -IHttpFsPkg/Include

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
