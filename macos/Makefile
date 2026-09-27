# Rufus for macOS - builds with plain make + Apple clang (Command Line Tools are enough)
#
#   make            build build/Rufus.app (universal: Intel + Apple Silicon)
#   make cli        build build/rufus-cli (engine test tool)
#   make run        build and launch the app, with the log in the terminal
#   make debug      build and launch the app under lldb
#   make dmg        build a distributable disk image
#   make clean
#
# Override the architectures with e.g. `make ARCHS=x86_64`.

SRC      := ../src
RES      := ../res
OUT      := build
ARCHS    ?= x86_64 arm64
MINOS    ?= 10.15
# Keep separate object trees per architecture set, so that switching between e.g.
# `make ARCHS=x86_64` and a universal build never mixes single and multi-arch objects
ARCHTAG  := $(subst $() ,-,$(strip $(ARCHS)))
OBJ      := $(OUT)/obj-$(ARCHTAG)
NTFS3G   := $(OUT)/ntfs3g-$(ARCHTAG)
VERSION  := $(shell sed -n 's/^\#define RUFUS_MAC_VERSION *"\(.*\)"/\1/p' core/rufus_core.h)
APP      := $(OUT)/Rufus.app

CC       := clang
ARCHFLAGS:= $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=$(MINOS)
CFLAGS   := $(ARCHFLAGS) -O2 -g -Wall -Wno-unused -Wno-sign-compare -Wno-pointer-sign -Wno-unknown-pragmas \
            -Wno-deprecated-declarations
LIBFLAGS := $(CFLAGS) -w -I compat
export MACOSX_DEPLOYMENT_TARGET := $(MINOS)

all: app

# ---- Rufus' bundled, portable libraries ------------------------------------
BLED_SRC   := $(wildcard $(SRC)/bled/*.c)
CDIO_SRC   := $(wildcard $(SRC)/libcdio/iso9660/*.c) $(wildcard $(SRC)/libcdio/udf/*.c) $(wildcard $(SRC)/libcdio/driver/*.c)
MSSYS_SRC  := $(filter-out %/file.c,$(wildcard $(SRC)/ms-sys/*.c))
LIBFAT_SRC := $(wildcard $(SRC)/syslinux/libfat/*.c)
SLINST_SRC := $(wildcard $(SRC)/syslinux/libinstaller/*.c)
EXT2_SRC   := $(filter-out %/nt_io.c,$(wildcard $(SRC)/ext2fs/*.c))
LIBS_OBJ   := $(patsubst $(SRC)/%.c,$(OBJ)/%.o,$(BLED_SRC) $(CDIO_SRC) $(MSSYS_SRC) $(LIBFAT_SRC) $(SLINST_SRC) $(EXT2_SRC))
EXT2FLAGS  := $(LIBFLAGS) -I $(SRC)/ext2fs -I $(SRC) -DEXT2_FLAT_INCLUDES=0 -DHAVE_CONFIG_H

$(OBJ)/bled/%.o: $(SRC)/bled/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) -c $< -o $@

$(OBJ)/libcdio/%.o: $(SRC)/libcdio/%.c compat/libcdio/config.h
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) -DHAVE_CONFIG_H -I compat/libcdio -I $(SRC)/libcdio -I $(SRC)/libcdio/driver -c $< -o $@

$(OBJ)/ms-sys/%.o: $(SRC)/ms-sys/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) -I $(SRC)/ms-sys/inc -c $< -o $@

$(OBJ)/syslinux/%.o: $(SRC)/syslinux/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBFLAGS) -I $(SRC)/syslinux -I $(SRC)/syslinux/libfat -I $(SRC)/syslinux/libinstaller -c $< -o $@

$(OBJ)/ext2fs/%.o: $(SRC)/ext2fs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(EXT2FLAGS) -c $< -o $@

$(OBJ)/librufuslibs.a: $(LIBS_OBJ)
	@rm -f $@
	ar rcs $@ $^

# ---- FatFs ------------------------------------------------------------------
FATFS_DIR  := third_party/fatfs/source
FATFS_OBJ  := $(OBJ)/fatfs/ff.o $(OBJ)/fatfs/ffsystem.o $(OBJ)/fatfs/ffunicode.o

$(OBJ)/fatfs/%.o: $(FATFS_DIR)/%.c $(FATFS_DIR)/ffconf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

# ---- ntfs-3g (library only, built from the MacPorts source tarball) ----------
NTFS_SRCDIR := third_party/ntfs-3g-2026.9.18
NTFS_LIB    := $(NTFS3G)/lib/libntfs-3g.a

$(NTFS_LIB):
	@# ac_cv_header_libintl_h=no: never pick up a libintl.h from /usr/local (e.g. Homebrew's gettext),
	@# which would redirect setlocale() to a libintl we don't bundle
	cd $(NTFS_SRCDIR) && CFLAGS="$(ARCHFLAGS) -O2" ac_cv_header_libintl_h=no ./configure --disable-dependency-tracking \
		--disable-ntfs-3g --disable-shared --enable-static --disable-crypto --disable-plugins \
		--without-uuid --without-hd --prefix=$(abspath $(NTFS3G)) >/dev/null
	$(MAKE) -C $(NTFS_SRCDIR)/libntfs-3g clean >/dev/null   # it builds in-tree: drop other archs' objects
	$(MAKE) -C $(NTFS_SRCDIR)/libntfs-3g
	@# ntfs-3g's install hook wants to move shared libraries to /lib: just copy what we need
	mkdir -p $(NTFS3G)/lib $(NTFS3G)/include/ntfs-3g
	cp $(NTFS_SRCDIR)/libntfs-3g/.libs/libntfs-3g.a $(NTFS_LIB)
	cp $(NTFS_SRCDIR)/include/ntfs-3g/*.h $(NTFS3G)/include/ntfs-3g/

# ---- The engine --------------------------------------------------------------
CORE_INC  := -I core -I $(FATFS_DIR) -I $(SRC)/ms-sys/inc -I $(SRC)/syslinux/libfat -I $(SRC)/syslinux/libinstaller \
             -I $(SRC)/libcdio -I $(NTFS3G)/include -DDO_NOT_WANT_COMPATIBILITY
CORE_OBJ  := $(patsubst core/%.c,$(OBJ)/core/%.o,$(wildcard core/*.c)) $(OBJ)/coreext/fs_ext.o

$(OBJ)/core/%.o: core/%.c core/rufus_core.h | $(NTFS_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CORE_INC) -c $< -o $@

# The ext code must see libext2fs with the exact same configuration as the library
$(OBJ)/coreext/%.o: core/ext/%.c core/rufus_core.h
	@mkdir -p $(dir $@)
	$(CC) $(EXT2FLAGS) -I core -c $< -o $@

$(OBJ)/librufuscore.a: $(CORE_OBJ) $(FATFS_OBJ)
	@rm -f $@
	ar rcs $@ $^

LINK_LIBS := $(OBJ)/librufuscore.a $(OBJ)/librufuslibs.a $(NTFS_LIB) -lz -liconv -framework CoreFoundation

# ---- rufus-mkntfs: ntfs-3g's mkntfs on our fd+offset device driver -----------
MKNTFS_OBJ := $(patsubst %,$(OBJ)/mkntfs/%.o,attrdef boot sd mkntfs utils)

$(OBJ)/mkntfs/%.o: $(NTFS_SRCDIR)/ntfsprogs/%.c | $(NTFS_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -DHAVE_CONFIG_H -Dntfs_device_unix_io_ops=rufus_ntfs_io_ops \
		-I $(NTFS_SRCDIR) -I $(NTFS_SRCDIR)/include/ntfs-3g -c $< -o $@

$(OBJ)/rufus-mkntfs: $(MKNTFS_OBJ) $(OBJ)/core/ntfs_io.o $(NTFS_LIB)
	$(CC) $(CFLAGS) $^ -framework CoreFoundation -o $@

# ---- Command line front-end ----------------------------------------------------
$(OBJ)/rufus-cli: cli/rufus_cli.c $(OBJ)/librufuscore.a $(OBJ)/librufuslibs.a $(OBJ)/rufus-mkntfs
	$(CC) $(CFLAGS) $(CORE_INC) cli/rufus_cli.c $(LINK_LIBS) -o $@

# rufus-cli needs rufus-mkntfs next to it
cli: $(OBJ)/rufus-cli $(OBJ)/rufus-mkntfs
	cp $(OBJ)/rufus-cli $(OBJ)/rufus-mkntfs $(OUT)/

# ---- The application -----------------------------------------------------------
APP_SRC  := $(wildcard app/*.m)
APP_OBJ  := $(patsubst app/%.m,$(OBJ)/app/%.o,$(APP_SRC))
APP_RES  := loc/rufus.loc syslinux/ldlinux_v4.sys syslinux/ldlinux_v4.bss syslinux/ldlinux_v6.sys syslinux/ldlinux_v6.bss \
            syslinux/mboot.c32 grub/grldr.mbr grub2/core.img mbr/msg.txt uefi/uefi-ntfs.img \
            setup/setup_x64.exe setup/setup_arm64.exe rufus.ico $(patsubst $(RES)/%,%,$(wildcard $(RES)/freedos/*.*))

$(OBJ)/app/%.o: app/%.m app/*.h core/rufus_core.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -fmodules $(CORE_INC) -I app -c $< -o $@

$(OBJ)/Rufus: $(APP_OBJ) $(OBJ)/librufuscore.a $(OBJ)/librufuslibs.a
	$(CC) $(CFLAGS) $(APP_OBJ) $(LINK_LIBS) -framework Cocoa -framework DiskArbitration -o $@

app: $(OBJ)/Rufus $(OBJ)/rufus-mkntfs app/Info.plist app/Rufus.icns
	@rm -rf $(APP)
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources/res
	cp $(OBJ)/Rufus $(OBJ)/rufus-mkntfs $(APP)/Contents/MacOS/
	sed 's/@VERSION@/$(VERSION)/g' app/Info.plist > $(APP)/Contents/Info.plist
	cp app/Rufus.icns $(APP)/Contents/Resources/
	@for f in $(APP_RES); do mkdir -p $(APP)/Contents/Resources/res/$$(dirname $$f); cp "$(RES)/$$f" "$(APP)/Contents/Resources/res/$$f"; done
	cp ../LICENSE.txt $(APP)/Contents/Resources/res/LICENSE.txt
	codesign --force --sign - --timestamp=none $(APP)/Contents/MacOS/rufus-mkntfs
	codesign --force --sign - --timestamp=none $(APP)
	@echo "Built $(APP) ($(ARCHS))"

run: app
	$(APP)/Contents/MacOS/Rufus

debug: app
	lldb -o run $(APP)/Contents/MacOS/Rufus

dmg: app
	rm -f $(OUT)/Rufus-$(VERSION).dmg
	hdiutil create -volname "Rufus $(VERSION)" -srcfolder $(APP) -ov -format UDZO $(OUT)/Rufus-$(VERSION).dmg

clean:
	rm -rf $(OUT)

.PHONY: all app cli run debug dmg clean
