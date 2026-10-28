# MinUI with PSP

A fork of [MinUI](https://github.com/shauninman/MinUI) that adds `PSP.pak`: PPSSPP running inside minarch, so PSP games get MinUI's in-game menu, save states and auto-resume.

Aimed for devices that use MinUI's `rg35xxplus` folder (H700: eg. RG35XX Plus, RG28XX, RG34XX, RG CubeXX). Tested on the RG28XX with Puzzle Quest: Challenge of the Warlords and Persona 3 Portable.
<img width="1228" height="921" alt="092E8A46-67B8-442A-BEE6-B231F4D877AF_1_102_o" src="https://github.com/user-attachments/assets/e4e6ad6b-bd56-4bcf-97a6-661f2c9391c9" />


## Install

1. Unzip `PSP.pak-rg35xxplus.zip` from [Releases](../../releases) at the root of your MinUI SD card.
2. Put `.iso`, `.cso`, `.pbp` or `.chd` games in `/Roms/Sony PlayStation Portable (PSP)/`.

No BIOS needed. The first launch copies PPSSPP's support files to `/Bios/PSP/PPSSPP`. Saves go to `/Saves/PSP`.

Defaults: fullscreen scaling, MENU+UP saves state, MENU+DOWN loads it. Changes made in the in-game menu only stick after Options → Save Changes.

If something goes wrong, point your agent at `/.userdata/rg35xxplus/logs/PSP.txt`.

## How it works

MinUI is 32-bit on these devices, but the stock GPU driver is 64-bit only. The pak ships its own 64-bit minarch (same source), MinUI's SDL2 fork and libmsettings; the rest of MinUI is untouched.

PPSSPP renders with OpenGL ES 2 into a framebuffer minarch owns. minarch reads each frame back into its normal pipeline, so scaling, effects, the menu, state thumbnails, HDMI and RG28XX rotation work as usual.

## Building

Needs Docker on an arm64 host (Apple Silicon or arm64 Linux).

    make psp64 PLATFORM=rg35xxplus

Output: `workspace/rg35xxplus/psp64/output/PSP.pak-rg35xxplus.zip`.

The build checks the binaries against the stock firmware's libraries and boots homebrew through both renderers; it fails if anything is off. Inputs are pinned: PPSSPP `v1.20.4` plus one patch (`workspace/all/cores/patches/ppsspp`), the SDL fork commit, libretro-common, and the Ubuntu image digest.

Code: `workspace/rg35xxplus/psp64` (build) and `workspace/all/minarch/minarch.c` (GLES support, `HAS_GLES` only).

## License

This fork's changes are MIT, see [LICENSE](LICENSE). Upstream MinUI code keeps its original terms. The release zip includes PPSSPP (GPL-2.0-or-later, built from the `v1.20.4` tag plus the patch above) and SDL (zlib).
