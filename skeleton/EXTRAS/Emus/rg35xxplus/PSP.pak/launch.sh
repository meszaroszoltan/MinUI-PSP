#!/bin/sh

EMU_EXE=ppsspp
PAK_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$PAK_PATH" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"

# ppsspp reads its assets from <system>/PPSSPP, sync the bundled ones when the pak version changes
ASSETS="$BIOS_PATH/$EMU_TAG/PPSSPP"
if ! cmp -s "$PAK_PATH/assets/.version" "$ASSETS/.version"; then
	mkdir -p "$ASSETS"
	cp -r "$PAK_PATH/assets/." "$ASSETS/"
fi

# 64-bit stack: our SDL/msettings first, then stock mali (/usr/lib) before glvnd/mesa
export LD_LIBRARY_PATH="$PAK_PATH/lib:/usr/lib:/usr/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH"
export SDL_VIDEO_EGL_DRIVER=/usr/lib/libEGL.so.1
export SDL_VIDEO_GL_DRIVER=/usr/lib/libGLESv2.so.2

HOME="$USERDATA_PATH"
cd "$HOME"
"$PAK_PATH/minarch64.elf" "$PAK_PATH/${EMU_EXE}_libretro.so" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1
