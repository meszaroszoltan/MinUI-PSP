# PSP (PPSSPP libretro) in minarch on rg35xxplus / RG28xx — implementation plan

Written for a Sonnet implementer. Everything below has been researched already. **Do not ask the user to check anything on a device.** They only test the final deliverable. Where something is uncertain, the plan gives a fallback.

## Context
Goal: run PSP games through **minarch**, the same frontend as every other emulator, so PSP gets MinUI's in-game menu (save/load states with thumbnails, options, disc/quit, auto-resume). Target: the `rg35xxplus` platform (Allwinner H700, 4×A53, Mali-G31, 1 GB), which also covers the RG28xx (portrait panel, `RGXX_MODEL`) and the other H700 models.

The user asked to "do what the other cores do". The other cores draw in software and hand minarch an RGB565 buffer. PPSSPP's software renderer is far too slow for PSP 3D on A53s, so this plan adds **GLES hardware rendering with readback**. The core renders on the GPU, and minarch reads the finished frame back into its normal RGB565 pipeline. That means the menu, scalers, screen effects, HDMI, RG28xx rotation, screenshots and thumbnails keep working unchanged. If GL setup fails at runtime, PPSSPP falls back to its software renderer and still works, just slowly.

## Research findings (these drive the design)
Sources: the stock sysroot tarball the union toolchain uses (`https://github.com/shauninman/union-rg35xxplus-toolchain/releases/download/sysroot-20240813-1/sysroot.tar.xz`, a dump of the stock rootfs with `mali_kbase.ko`, kernel 4.9.170), the union toolchain repo, and the PPSSPP master source.

1. **Current MinUI for rg35xxplus is 32-bit armhf**. Toolchain: Buildroot 2017.11, **GCC 7.2**, `arm-buildroot-linux-gnueabihf`. Its cores are copied from the cortex-a9 `rg35xx` build.
2. **The stock OS is Ubuntu-22.04-style multiarch: glibc 2.35, libstdc++ 6.0.30**. It has full aarch64 and armhf userlands.
3. **The GPU driver usable at link time is 64-bit.** `/usr/lib/libmali.so.0.20.0`, `/usr/lib/libEGL.so.1`, `/usr/lib/libGLESv2.so.2` are aarch64 ELF. libmali is the **fbdev** variant: it exports `egl_create_pixmap_ID_mapping`, `eglCreatePbufferSurface`, `glReadPixels`, `glBindFramebuffer`. The sysroot has no armhf libmali to link against. There are also glvnd/mesa `libEGL.so.1` and `libGLESv2.so.2` in `/usr/lib/aarch64-linux-gnu/`, so library search order matters.
4. The stock aarch64 userland also has `libSDL2_image-2.0.so.0`, `libSDL2_ttf-2.0.so.0`, `libasound.so.2`, `libz.so.1`, `libpng16`, `libfreetype` in `/usr/lib/aarch64-linux-gnu/`.
5. MinUI ships its own SDL2 (JohnnyonFlame's `SDL-malifbdev-rot`, built in `workspace/rg35xxplus/makefile` with `--enable-video-mali`). It handles RG28xx rotation, and it needs a Mali fbdev EGL exporting `egl_create_pixmap_ID_mapping`, which the 64-bit libmali does.
6. **PPSSPP needs C++17**, and GCC 7.2 can't build modern PPSSPP. That (plus libstdc++ mismatch) is the old "missing libraries / unlinked function" failure in `todo.txt:449`.
7. PPSSPP libretro, from the source:
   - The GLES build requests `RETRO_HW_CONTEXT_OPENGLES2`, `depth = true`, `bottom_left_origin = true`.
   - It uses an internal emu thread. GL calls happen on the thread that calls `retro_run`.
   - It links `-lGLESv2 -lEGL` directly.
   - It requires `SET_PIXEL_FORMAT XRGB8888` to succeed, or load fails.
   - It reads assets from `<system_dir>/PPSSPP/` (`compat.ini`, `ppge_atlas.zim`, `flash0/font/*`).
   - Fallbacks run GL → Vulkan → `LibretroSoftwareContext`.
   - Use Makefile `platform=arm64_gles` (the `arm64` branch plus `gles` substring gives `-DUSING_GLES2 -lGLESv2 -lEGL`). **Never** a name containing `unix` (that branch wins first and loses the ARM flags).
8. `libmsettings`' shared-memory `Settings` struct is all `int`, so the layout is identical in 32 and 64 bit. A 64-bit minarch can share settings with the 32-bit `keymon.elf` that's already running.
9. minarch is already 64-bit clean (tg5040/my355/rgb30 are aarch64). The only 32-bit-only bit for rg35xxplus is `#define HAS_NEON` in `workspace/rg35xxplus/platform/platform.h:138`, which enables armv7 inline asm in `workspace/all/common/scaler.c:424`.
10. The host is Apple Silicon (`arm64`) with Docker, so an `ubuntu:22.04` arm64 container builds **natively** with matching glibc 2.35 and GCC 11. No cross-compiler is needed.

**Decision:** PSP gets its own **64-bit** stack inside `PSP.pak`: `minarch64.elf` (same minarch source, `PLATFORM=rg35xxplus`, so paths, configs and saves match), a 64-bit build of the same SDL fork, a 64-bit `libmsettings.so`, and a 64-bit `ppsspp_libretro.so`. The rest of MinUI stays 32-bit and untouched.

## Part A — 64-bit build environment
New directory `workspace/rg35xxplus/psp64/`:
- `Dockerfile`: `FROM --platform=linux/arm64 ubuntu:22.04`. apt: `build-essential git cmake pkg-config python3 zip libsdl2-image-dev libsdl2-ttf-dev libgles-dev libegl-dev libasound2-dev zlib1g-dev`. The `libsdl2-*-dev` packages pull in Ubuntu's libsdl2-dev; that's fine for headers, but we link and ship our own SDL fork. Set the env the existing makefiles expect: `CROSS_COMPILE=` (empty is not allowed by `workspace/all/minarch/makefile:12` and the libmsettings makefile), so use `CROSS_COMPILE=/usr/bin/`, `PREFIX=/opt/psp64` (mkdir `include`/`lib`), `UNION_PLATFORM=rg35xxplus`.
- `makefile` (run inside the container, mounted at `/root/workspace` like the union toolchains so `minarch/makefile:52`'s `/root/workspace/$(PLATFORM)/libmsettings` path still resolves). Targets, in order:
  1. `sdl2`: clone `https://github.com/JohnnyonFlame/SDL-malifbdev-rot` into `src/sdl2` (same repo as `workspace/rg35xxplus/makefile:62`; check whether the 32-bit build uses a branch, e.g. `rg28xx`, and match it). Build with the same `./configure` flags as `workspace/rg35xxplus/makefile:30-58`, but `--host=aarch64-linux-gnu`, CFLAGS `-O2 -mcpu=cortex-a53`, and **no** `--enable-arm-neon` (armv7-only). `make install DESTDIR=` into `$(PREFIX)`, so minarch compiles against these headers and this lib (`-I$(PREFIX)/include/SDL2 -L$(PREFIX)/lib` ahead of system paths).
  2. `msettings`: `cd ../libmsettings && make` with `CROSS_COMPILE`/`PREFIX` from the env. It installs `msettings.h` and `libmsettings.so` into `$(PREFIX)`. Output must not overwrite the 32-bit `.so` in the source dir: build in a copy, or `make clean` afterwards (`libmsettings/makefile` writes `libmsettings.so` in place).
  3. `minarch`: `make -C ../../all/minarch PLATFORM=rg35xxplus PLATFORM_ENV=../../rg35xxplus/psp64/makefile.env BUILD_DIR=build/rg35xxplus64` (see Part C for the makefile hooks).
  4. `ppsspp`: see Part B.
  5. `check`: see Verification. This **must** pass as part of `all`.
- `makefile.env` (64-bit variant of `workspace/rg35xxplus/platform/makefile.env`):
  ```
  ARCH = -mcpu=cortex-a53
  LIBS = -flto -lGLESv2 -L$(PREFIX)/lib -Wl,-rpath-link,$(PREFIX)/lib
  SDL = SDL2
  CFLAGS_EXTRA = -DHAS_GLES -I$(PREFIX)/include
  ```
  Wire `CFLAGS_EXTRA` in (Part C).
- Root `makefile`: add a `psp64` target, mirroring `makefile.toolchain`'s docker usage: `docker build --platform linux/arm64 -t minui-psp64 workspace/rg35xxplus/psp64` then `docker run --rm --platform linux/arm64 -v $(HOST_WORKSPACE):/root/workspace minui-psp64 make -C /root/workspace/rg35xxplus/psp64`. Run it from the `rg35xxplus` platform build, after `build`, before `cores`. In the `cores:` target, under `ifeq ($(PLATFORM), rg35xxplus)`, copy into `./build/EXTRAS/Emus/rg35xxplus/PSP.pak/`:
  - `minarch64.elf`
  - `lib/libSDL2-2.0.so.0` (the real file, not a symlink, because the release zip is FAT32)
  - `lib/libmsettings.so`
  - `ppsspp_libretro.so`
  - `assets/` (PPSSPP's `assets/` dir; see Part D)

## Part B — PPSSPP core build
- In `psp64/makefile`: clone `https://github.com/hrydgard/ppsspp` with `--recursive` (needs the `ffmpeg` and `ext/*` submodules). **Pin to the latest stable release tag** (`git ls-remote --tags`, pick the newest `vX.Y.Z` that isn't `-rc`). Record the tag in the makefile like other `_HASH` pins.
- Build: `cd src/ppsspp/libretro && make platform=arm64_gles -j$(nproc)`. Before building, open `libretro/Makefile` and `Makefile.common` and check three things:
  1. The `arm64` branch sets `TARGET_ARCH=arm64` and the ARM64 JIT sources (`Core/MIPS/ARM64/*`, `Common/Arm64Emitter.cpp`) are compiled, not the x86 ones.
  2. The `gles` substring gives `-DUSING_GLES2` and `-lGLESv2 -lEGL`.
  3. Prebuilt ffmpeg for `linux/aarch64` is found. If it's missing, build ffmpeg from the submodule's `linux_arm64.sh`, or disable ffmpeg (games with ATRAC3+ audio need it, so prefer building it).

  If the Makefile path is broken on that tag, use CMake from the repo root instead: `cmake -DLIBRETRO=ON -DUSING_GLES2=ON -DUSING_EGL=ON -DCMAKE_BUILD_TYPE=Release ..`.
- Add `-static-libstdc++ -static-libgcc` to `LDFLAGS` (via `make LDFLAGS+=...`, or a patch under `workspace/all/cores/patches/ppsspp/` if the Makefile overrides it). Then `strip` the result.
- The output is `ppsspp_libretro.so` (aarch64).

## Part C — minarch changes (`workspace/all/minarch/`)
**makefile:**
- `PLATFORM_ENV ?= ../../$(PLATFORM)/platform/makefile.env`, then `include $(PLATFORM_ENV)`.
- `BUILD_DIR ?= build/$(PLATFORM)`, used for `PRODUCT` and `mkdir`.
- `CFLAGS += $(CFLAGS_EXTRA)`.
- Defaults leave every existing platform build byte-identical.

**`workspace/rg35xxplus/platform/platform.h:138`:** change to `#ifdef __arm__` / `#define HAS_NEON` / `#endif`.

**minarch.c.** Match the file's style: tabs, `LOG_info`, terse comments, static globals. Put all GL code under `#ifdef HAS_GLES`.
1. **Pixel format** (`case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT`, ~line 1896): accept `RETRO_PIXEL_FORMAT_XRGB8888` and set `downsample = 1`. Keep RGB565 as is. Keep rejecting 0RGB1555.
2. **Finish the downsample path** (`buffer_downsample` ~2517, `buffer_realloc` ~2539, and the `pitch /= 2` hack in `video_refresh_callback_main` ~2803). Read `buffer_downsample` first. It must convert `uint32_t` 0x00RRGGBB to RGB565 (`((p>>8)&0xF800)|((p>>5)&0x07E0)|((p>>3)&0x1F)`) into `buffer` with a tightly packed 565 pitch. `renderer.src_p` / `selectScaler` must get the **565 pitch** (`width*2`), not the 8888 pitch. Fix the pitch bookkeeping so `selectScaler(width,height,pitch565)` is called and the debug overlay draws into the 565 buffer **after** conversion; currently it draws into `data` before conversion, which is wrong for 8888. This is the software fallback path, and the HW path below reuses it.
3. **Threaded video**: if `downsample` or HW render is active, force `thread_video = 0`, and ignore the toggle shortcut (~4779) for that session. The threaded backbuffer path assumes 565 + `memcpy` from `data`, and PPSSPP already runs its own emu thread.
4. **HW render state** (new static struct near the other globals):
   ```c
   #ifdef HAS_GLES
   static struct {
   	int enabled;
   	struct retro_hw_render_callback cb;
   	SDL_GLContext ctx;      // core's context
   	SDL_GLContext prev;     // SDL_Renderer's context
   	GLuint fbo, tex, rb;
   	int fbo_w, fbo_h;
   	uint8_t* readback;      // fbo_w*fbo_h*4
   } hw;
   #endif
   ```
5. **Environment**:
   - `RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER` (56): `*(unsigned*)data = RETRO_HW_CONTEXT_OPENGLES2; return true;`
   - `RETRO_ENVIRONMENT_SET_HW_RENDER` (14): accept `OPENGLES2`, `OPENGLES3`, and `OPENGLES_VERSION` with major ≤ 3. Copy the struct into `hw.cb`, set `cb->get_current_framebuffer = hw_get_current_framebuffer` (returns `hw.fbo`) and `cb->get_proc_address = hw_get_proc_address` (wraps `SDL_GL_GetProcAddress`), set `hw.enabled = 1`, return true. Reject everything else (return false). PPSSPP then falls back to software and still runs.
   - `RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT` (44): return true.
   - Define missing constants locally if the bundled `libretro.h` lacks them. It should have them; check `workspace/all/minarch/libretro-common/include/libretro.h`.
6. **Context creation**: `HW_init()`, called in `main()` after `Core_load()` and once the av_info geometry is known. Store `max_width`/`max_height` from `get_system_av_info` in `core` (add fields) inside `Core_load`.
   - `hw.prev = SDL_GL_GetCurrentContext()`, which is SDL_Renderer's GLES2 context, created by `SDL_CreateRenderer` in platform.c.
   - `SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES)`, `MAJOR 2` (or the requested major, 3 if OPENGLES3), `MINOR 0`. Then `hw.ctx = SDL_GL_CreateContext(vid window)`. The window is `vid.window` inside platform.c, so add a tiny platform accessor `SDL_Window* PLAT_getWindow(void)`. Declare it in `workspace/all/common/api.h` and implement it in `workspace/rg35xxplus/platform/platform.c` only, under `#ifdef HAS_GLES` in the header so other platforms needn't implement it.
   - Restore the attributes SDL_Renderer used if they were changed. Creating a context makes it current, so immediately `SDL_GL_MakeCurrent(window, hw.ctx)` to build the FBO.
   - FBO: `glGenTextures` RGBA `GL_UNSIGNED_BYTE`, `fbo_w × fbo_h` (= max geometry), `GL_NEAREST`. Renderbuffer `GL_DEPTH24_STENCIL8_OES` (check the `GL_OES_packed_depth_stencil` extension; Mali-G31 has it). Fall back to `GL_DEPTH_COMPONENT16` if it's missing. Attach it to `GL_DEPTH_ATTACHMENT`, and also to `GL_STENCIL_ATTACHMENT` when packed. Check `glCheckFramebufferStatus`.
   - Call `hw.cb.context_reset()` while `hw.ctx` is current.
   - `SDL_GL_MakeCurrent(window, hw.prev)`.
   - Order (same as RetroArch): the env callback only stores the request during `retro_load_game`. After `load_game` returns, `HW_init()` creates the context and FBO and calls `context_reset`.
   - On failure: log it, free what was created, and set `hw.enabled = 0`. Because the core already chose GL, it will produce no frames, so `LOG_error` clearly and exit back to MinUI with an on-screen error. Use the same path minarch uses when a core fails to load. Don't hang on a black screen.
7. **Run loop**: wrap both `core.run()` call sites (~4660, ~4754) as `HW_begin(); core.run(); HW_end();`. Begin is `SDL_GL_MakeCurrent(win, hw.ctx)`; end is `SDL_GL_MakeCurrent(win, hw.prev)`; both are no-ops when `!hw.enabled`. SDL's GLES2 renderer re-binds its own context when `SDL_GL_GetCurrentContext()` differs, and restoring explicitly avoids depending on that.
8. **Frame delivery** (`video_refresh_callback`, ~2848): if `data == RETRO_HW_FRAME_BUFFER_VALID && hw.enabled`:
   - `glBindFramebuffer(GL_FRAMEBUFFER, hw.fbo)`, `glPixelStorei(GL_PACK_ALIGNMENT, 4)`, `glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,hw.readback)`. This happens while `hw.ctx` is still current, because video_refresh is called from inside `core.run()`.
   - Convert RGBA bytes to RGB565 into `buffer` with a **vertical flip** when `hw.cb.bottom_left_origin`: `r=p[0], g=p[1], b=p[2]` gives `((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)`. Reuse `buffer_realloc` for sizing.
   - Call `video_refresh_callback_main(buffer, width, height, width*2)` with a flag so it skips the downsample branch, e.g. a local `static int hw_frame` or a parameter split. Keep everything after that (scaler, debug text, `GFX_blitRenderer`, `GFX_flip`) unchanged.
   - If `data == RETRO_HW_FRAME_BUFFER_VALID` and fast-forward wants to skip (`SDL_GetTicks()-last_flip_time<10`), skip the readback too. Move that check before the readback.
   - `data == NULL` (dupe) stays a return.
9. **Teardown**: in `Core_quit`, before `core.unload_game()`, call `HW_begin(); hw.cb.context_destroy(); HW_end();` if set. After unload, delete the FBO, texture and renderbuffer, then `SDL_GL_DeleteContext(hw.ctx)`.
10. **Menu/thumbnails**: nothing to do. `renderer.src` points at the converted 565 `buffer`, which is what `Menu_loop` and the save-state bitmap (~4146) use.
11. **Core options**: minarch reports `GET_CORE_OPTIONS_VERSION = 1` (~2040). PPSSPP's `libretro_core_options.h` helper converts v2 to v1 automatically, so no change is needed. Confirm the option list shows up.
12. **Sleep/power-off**: `Menu_beforeSleep` / `Menu_afterSleep` stay as is. The GL context survives fb blanking on Mali fbdev the same way SDL_Renderer's does.

## Part D — PSP.pak and assets
`skeleton/EXTRAS/Emus/rg35xxplus/PSP.pak/`:
- `launch.sh`, based on `skeleton/EXTRAS/Emus/rg35xxplus/MGBA.pak/launch.sh`:
  ```sh
  #!/bin/sh
  EMU_EXE=ppsspp
  PAK_PATH=$(dirname "$0")
  EMU_TAG=$(basename "$PAK_PATH" .pak)
  ROM="$1"
  mkdir -p "$BIOS_PATH/$EMU_TAG" "$SAVES_PATH/$EMU_TAG"
  # PPSSPP reads <system>/PPSSPP; sync bundled assets when the pak version changes
  ASSETS="$BIOS_PATH/$EMU_TAG/PPSSPP"
  if ! cmp -s "$PAK_PATH/assets/.version" "$ASSETS/.version"; then
  	mkdir -p "$ASSETS" && cp -r "$PAK_PATH/assets/." "$ASSETS/"
  fi
  # 64-bit stack: our SDL/msettings first, then stock Mali (/usr/lib) before glvnd/mesa
  export LD_LIBRARY_PATH="$PAK_PATH/lib:/usr/lib:/usr/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH"
  export SDL_VIDEO_EGL_DRIVER=/usr/lib/libEGL.so.1
  export SDL_VIDEO_GL_DRIVER=/usr/lib/libGLESv2.so.2
  HOME="$USERDATA_PATH"
  cd "$HOME"
  "$PAK_PATH/minarch64.elf" "$PAK_PATH/${EMU_EXE}_libretro.so" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1
  ```
  The 32-bit `$SYSTEM_PATH/lib` in the inherited `LD_LIBRARY_PATH` is harmless, because the loader skips wrong-class ELFs. Our 64-bit libs come first.
- `default.cfg`, matching the key style of other paks' `default.cfg` (check one with core options, e.g. `skeleton/SYSTEM/rg35xxplus/paks/Emus/PS.pak/default.cfg`):
  - `ppsspp_internal_resolution = 480x272`
  - `ppsspp_frameskip = 0`
  - `ppsspp_auto_frameskip = disabled`
  - `ppsspp_fast_memory = enabled`
  - `ppsspp_cpu_core = JIT`
  - `ppsspp_software_rendering = disabled`
  - `ppsspp_texture_scaling_level = 1`
  - `ppsspp_button_preference = Cross`
  - `minarch_thread_video = 0` if that key exists in minarch's frontend options; check how minarch names frontend options in `minarch.c`

  Use the exact option keys from the pinned PPSSPP's `libretro/libretro_core_options.h`. Add `default-wide.cfg` / `default-cube.cfg` only if other EXTRAS paks need them (MGBA has them), copying the same values.
- `assets/`: copy `src/ppsspp/assets/*` at build time (it holds `compat.ini`, `ppge_atlas.zim`, `flash0/`, `lang/` and so on). Leave out `shaders/` and `ui_images` if they're large and unused by libretro. Write `.version` containing the PPSSPP tag.
- Roms folder: add `Roms/Sony PlayStation Portable (PSP)` the way other EXTRAS systems are declared. Check how EXTRAS roms folders are created, e.g. `skeleton/EXTRAS/Roms/` or the README, and copy the convention for MGBA/PCE.
- README: add PSP to the Extras list in `README.md` and `skeleton/EXTRAS/README.txt`. Note that it's H700/rg35xxplus-family only and that ISO/CSO/PBP/CHD are supported.
- `PAKS.md`: replace the "does not implement the OpenGL libretro APIs" caveat with a short note that minarch supports GLES2/3 hw-render cores (readback) on GLES-enabled builds.
- `todo.txt`: update the PSP note.

## Critical files
- `workspace/all/minarch/minarch.c`: env callback, pixel format, downsample, video refresh, Core_load/Core_quit, run loop, thread_video guard
- `workspace/all/minarch/makefile`: `PLATFORM_ENV`, `BUILD_DIR`, `CFLAGS_EXTRA` hooks
- `workspace/all/common/api.h`, `workspace/rg35xxplus/platform/platform.c`: `PLAT_getWindow`
- `workspace/rg35xxplus/platform/platform.h`: `HAS_NEON` guard
- New: `workspace/rg35xxplus/psp64/{Dockerfile,makefile,makefile.env}`
- Root `makefile`: `psp64` target, PSP.pak copy lines
- New: `skeleton/EXTRAS/Emus/rg35xxplus/PSP.pak/*`, Roms folder, READMEs, `PAKS.md`, `todo.txt`

## Verification (no device needed; the user tests the final zip)
Automate this as `make check` in `psp64/makefile`; it fails the build on any error:
1. **Arch/ABI**: `file` shows aarch64 for `minarch64.elf`, `ppsspp_libretro.so`, `libSDL2-2.0.so.0`, `libmsettings.so`.
2. **Dependencies exist on stock**: download and extract the stock sysroot tarball (URL above) into `psp64/.sysroot/`, aarch64 dirs only (`usr/lib/*.so*`, `usr/lib/aarch64-linux-gnu/`, `lib/aarch64-linux-gnu/`, `usr/lib/ld-linux-aarch64.so.1`). Every `DT_NEEDED` from `readelf -d` of the four binaries must be either in `PSP.pak/lib` or present in the sysroot. Then run `LD_LIBRARY_PATH=<pak lib>:.sysroot/usr/lib:.sysroot/usr/lib/aarch64-linux-gnu ldd -r` on each file inside the arm64 container. **Zero "undefined symbol" lines** is the gate that catches the old "unlinked function" failure.
3. **Symbol versions**: `objdump -T` shows no `GLIBC_` newer than `2.35` and no `GLIBCXX_`/`CXXABI_` requirement at all for ppsspp (static libstdc++).
4. **Core load smoke test in the container**: a tiny `psp64/smoke.c` that `dlopen`s `ppsspp_libretro.so`, calls `retro_set_environment` with a stub env (system dir pointed at a temp dir with the assets; `SET_HW_RENDER` → false, so software path; `SET_PIXEL_FORMAT` → true), then `retro_init`, `retro_load_game` on a tiny homebrew PBP, ~120 × `retro_run`. Assert that video_cb receives non-null XRGB8888 frames of 480×272. Use a CC0 homebrew, or generate one if PPSSPP's repo has a test PBP under `pspautotests` (submodule). If no ROM is available, at least assert `retro_init` and `retro_get_system_info` succeed. Also run the XRGB8888→565 downsample converter over one captured frame in a unit test and compare against a reference 565 conversion.
5. **Regression**: run a full `make PLATFORM=rg35xxplus` and `make PLATFORM=tg5040`. The 32-bit `minarch.elf` must still build. `HAS_GLES` must be undefined there (check with `strings minarch.elf | grep -c SDL_GL_CreateContext` → 0 for 32-bit).
6. Final deliverable for the user to test on the RG35XX Plus / RG28xx:
   - A PSP game launches.
   - The in-game menu opens with the game frame as backdrop.
   - Save and load state work, with a thumbnail.
   - Options lists the PPSSPP options.
   - Quit, then relaunch, and resume works.
   - RG28xx shows the correct orientation.
   - Other systems are unaffected.
