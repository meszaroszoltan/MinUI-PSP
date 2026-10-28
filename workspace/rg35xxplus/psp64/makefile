# rg35xxplus 64-bit stack for PSP.pak (minarch64, SDL2, msettings, ppsspp)
# runs inside the minui-psp64 docker image, see the psp64 target in the root makefile

ifeq (,$(PREFIX))
$(error missing PREFIX, run this inside the minui-psp64 docker image)
endif

###########################################################

PPSSPP_TAG = v1.20.4
# rg28xx branch, same repo as ../makefile
SDL2_HASH = d4a7d7503524cc469fe775242f3f925d4dd56c88
LIBRETRO_COMMON_HASH = c0d33c5f176f31ab23be98d7ea4a985e8a10c2da
SYSROOT_URL = https://github.com/shauninman/union-rg35xxplus-toolchain/releases/download/sysroot-20240813-1/sysroot.tar.xz

NPROC := $(shell nproc)
OUT = output

# the pak's launch.sh and default.cfg live in the skeleton, mounted by the root makefile
PAK_SRC ?= /root/skeleton/EXTRAS/Emus/rg35xxplus/PSP.pak
ZIP = PSP.pak-rg35xxplus.zip

###########################################################

.PHONY: all sdl2 msettings minarch ppsspp assets check pak clean

all: sdl2 msettings minarch ppsspp assets check pak

src/sdl2:
	git clone https://github.com/JohnnyonFlame/SDL-malifbdev-rot src/sdl2
	cd src/sdl2 && git checkout $(SDL2_HASH)

# minarch's own makefile clones this unpinned if it's missing
../../all/minarch/libretro-common:
	git clone https://github.com/libretro/libretro-common $@
	cd $@ && git checkout $(LIBRETRO_COMMON_HASH)

src/ppsspp:
	git clone --depth 1 --branch $(PPSSPP_TAG) --recursive --shallow-submodules https://github.com/hrydgard/ppsspp src/ppsspp

src/ppsspp/.patched: | src/ppsspp
	cd src/ppsspp && $(foreach patch, $(sort $(wildcard ../../all/cores/patches/ppsspp/*.patch)), patch -p1 < ../../$(patch) &&) touch .patched

# same flags as the 32-bit build in ../makefile minus --enable-arm-neon (armv7 only)
# desktop GL and GLES1 are off so SDL_Renderer always picks GLES2 (ubuntu has the headers, the 32-bit sysroot doesn't)
build/sdl2/Makefile: | src/sdl2
	mkdir -p build/sdl2
	cd build/sdl2 && CFLAGS="-O2 -mcpu=cortex-a53 -ffast-math -ftree-vectorize" \
		../../src/sdl2/configure CC=gcc --host=aarch64-linux-gnu --prefix=$(PREFIX) \
		  --disable-oss \
		  --disable-rpath \
		  --disable-arts \
		  --disable-esd \
		  --disable-dbus \
		  --disable-diskaudio \
		  --disable-dummyaudio \
		  --disable-pulseaudio \
		  --disable-pipewire \
		  --disable-jack \
		  --disable-sndio \
		  --disable-video-wayland \
		  --disable-video-kmsdrm \
		  --disable-video-vivante \
		  --disable-video-cocoa \
		  --disable-video-metal \
		  --disable-video-dummy \
		  --disable-video-offscreen \
		  --disable-video-x11 \
		  --disable-video-vulkan \
		  --disable-video-opengl \
		  --disable-video-opengles1 \
		  --disable-ime \
		  --disable-ibus \
		  --disable-fcitx \
		  --disable-joystick-mfi \
		  --disable-directx \
		  --disable-xinput \
		  --disable-wasapi \
		  --disable-hidapi-joystick \
		  --disable-hidapi-libusb \
		  --disable-joystick-virtual \
		  --enable-video-mali \
		  --disable-libsamplerate

sdl2: build/sdl2/Makefile
	cd build/sdl2 && make -j$(NPROC) && make install
	mkdir -p $(OUT)/lib
	cp -L $(PREFIX)/lib/libSDL2-2.0.so.0 $(OUT)/lib/libSDL2-2.0.so.0

# built in a copy so the 32-bit libmsettings.so in ../libmsettings is left alone
msettings:
	rm -rf build/libmsettings
	mkdir -p build $(OUT)/lib
	cp -R ../libmsettings build/libmsettings
	cd build/libmsettings && make
	cp build/libmsettings/libmsettings.so $(OUT)/lib/

minarch: | ../../all/minarch/libretro-common
	make -C ../../all/minarch PLATFORM=rg35xxplus PLATFORM_ENV=../../rg35xxplus/psp64/makefile.env BUILD_DIR=build/rg35xxplus64
	mkdir -p $(OUT)
	cp ../../all/minarch/build/rg35xxplus64/minarch.elf $(OUT)/minarch64.elf

# flags from the environment so the Makefile's own += still apply
# libpng17's neon filters aren't in Makefile.common's sources, png is only used for texture packs anyway
ppsspp: src/ppsspp/.patched
	cd src/ppsspp/libretro && \
		CFLAGS="-mcpu=cortex-a53 -DPNG_ARM_NEON_OPT=0" CXXFLAGS="-mcpu=cortex-a53" LDFLAGS="-static-libstdc++ -static-libgcc" \
		make platform=arm64_gles -j$(NPROC)
	mkdir -p $(OUT)
	cp src/ppsspp/libretro/ppsspp_libretro.so $(OUT)/
	strip $(OUT)/ppsspp_libretro.so

# copied to <system>/PPSSPP by launch.sh, minus the desktop ui bits libretro never reads
assets: | src/ppsspp
	rm -rf $(OUT)/assets
	mkdir -p $(OUT)/assets
	cp -R src/ppsspp/assets/. $(OUT)/assets/
	cd $(OUT)/assets && rm -rf debugger ui_images sfx_*.wav *.ttf gamecontrollerdb.txt
	echo $(PPSSPP_TAG) > $(OUT)/assets/.version

###########################################################

# stock rootfs (aarch64 side only) for check.sh
.sysroot/usr/lib/libmali.so:
	mkdir -p .dl .sysroot
	[ -f .dl/sysroot.tar.xz ] || curl -fL -o .dl/sysroot.tar.xz $(SYSROOT_URL)
	tar -xJf .dl/sysroot.tar.xz -C .sysroot --exclude='usr/lib/arm-linux-gnueabihf' usr/lib

build/smoke: smoke.c
	mkdir -p build
	gcc -O2 -std=gnu99 -Wall -I../../all/minarch/libretro-common/include -o $@ $< -ldl -lEGL -lGLESv2

check: .sysroot/usr/lib/libmali.so build/smoke
	./check.sh

# laid out like the sd card: unzip at the root
pak:
	rm -rf $(OUT)/sdcard $(OUT)/$(ZIP)
	mkdir -p $(OUT)/sdcard/Emus/rg35xxplus/PSP.pak/lib "$(OUT)/sdcard/Roms/Sony PlayStation Portable (PSP)"
	cp $(PAK_SRC)/launch.sh $(PAK_SRC)/default.cfg $(OUT)/minarch64.elf $(OUT)/ppsspp_libretro.so $(OUT)/sdcard/Emus/rg35xxplus/PSP.pak/
	cp $(OUT)/lib/libSDL2-2.0.so.0 $(OUT)/lib/libmsettings.so $(OUT)/sdcard/Emus/rg35xxplus/PSP.pak/lib/
	cp -R $(OUT)/assets $(OUT)/sdcard/Emus/rg35xxplus/PSP.pak/
	cd $(OUT)/sdcard && zip -qrX ../$(ZIP) Emus Roms

clean:
	rm -rf build $(OUT)
	make -C ../../all/minarch PLATFORM=rg35xxplus PLATFORM_ENV=../../rg35xxplus/psp64/makefile.env BUILD_DIR=build/rg35xxplus64 clean
	cd src/ppsspp/libretro && make platform=arm64_gles clean
