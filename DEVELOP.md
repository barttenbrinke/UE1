# Developing the PSP port

The player-facing side (install, play, report) is in `README.md`. This file is for people who want
to build the port, understand what it changes, or work on it. `PSP-TODO.md` is the engineering log
of what was measured and changed, in detail.

## What it does

The engine is the 1998 code, compiled for the PSP's MIPS CPU, with the parts rewritten that the
console could not carry:

* **Streaming instead of resident data.** Textures, sounds and mesh geometry are read from the
  Memory Stick on first use and dropped again under memory pressure. A level that took 60 MB on a
  PC fits in the 40 MB the PSP-2000 leaves to a game.
* **Sound on the second CPU.** Sound effects are mixed and the tracker music is played on the
  Media Engine, the PSP's second MIPS core, through mcidclan's me-core library. The game CPU only
  hands over the voices.
* **Lighting folded into vertex colours.** The BSP lightmaps are baked into the geometry pass, one
  draw instead of two, with a gamma curve tuned for the PSP's LCD.
* **A fixed region for large allocations** so that level changes do not fragment the heap, and a
  loader tuned for the Memory Stick's slow seeks.

The comments at the top of `Source/NOpenALDrv/PspMix.h`, `Source/Core/Src/UnFile.cpp` and
`Source/NOpenGLDrv/NOpenGLDrv.cpp` explain the three big pieces.

## What works, and what is known not to

Confirmed on a PSP-2000 with custom firmware:

* The intro flyby, the Vortex Rikers and the following levels load and play; the first level has
  been completed on the console and the second loads after it.
* Saving from the menu and loading a save, including from inside a running level.
* Sound effects, music, the translator, weapons, inventory.
* Frame rate of about 20 to 30 fps in the early levels, dropping to 11 to 13 fps in the heaviest
  castle views. Level loads take 8 to 20 seconds from the Memory Stick.
* Clean exit to the XMB from the menu.

Known limits and open questions:

* **Later levels were only swept in the emulator** for memory use, not played on hardware.
* **Deathmatch runs** (bots included) and has been used for performance testing, but has hardly
  been played.
* **Suspending the PSP mid-game** with the power switch, and hub levels with several saves, are
  untested.
* **Scripted sequences with many actors** (the collapsing floor in Vortex Rikers, for example) still
  dip in frame rate, though far less than they did.
* The PSP-1000 with 32 MB of RAM is **not** supported; the game does not fit.
* Music volume against effects volume has not been balanced yet.

## Building

1. Install the [pspdev toolchain](https://github.com/pspdev/pspdev) (a release archive or the
   build script), set `PSPDEV` to its location and put `$PSPDEV/bin` on your `PATH`. The toolchain
   must provide SDL2, pspgl (`libGL`/`libGLU`) and libxmp for the PSP; the all-in-one releases do,
   otherwise install them with `psp-pacman` (`sdl2`, `pspgl`, `libxmp`). OpenAL is **not** needed:
   the PSP build has its own mixer behind a thin OpenAL-compatible shim.
2. Install the Media Engine core library, which lets the game run code on the PSP's second CPU:
   [`psp-media-engine-custom-core`](https://github.com/mcidclan/psp-media-engine-custom-core) by
   mcidclan. `make clean; make install` in its checkout puts `libme-core.a` and
   `<me-core-mapper/me-core.h>` into `$PSPDEV`. On macOS its build needs GNU sed
   (`brew install gnu-sed`); after a failed build, clean before retrying, or a stale embedded
   object survives.
3. Build:
   ```
   ./build-psp.sh              # build-psp/Unreal/EBOOT.PBP
   ./build-psp.sh --psplink    # additionally build-psplink/Unreal/Unreal.prx, for debugging over PSPLink
   ```
   The script runs CMake with the options the port expects (no editor, no networking, the fixed
   pipeline GL driver, one PRX). To configure by hand instead, read the `COMMON` list in the script.
4. Install with `./install-psp.sh` as above.

### Debugging on the console

The PSPLink build mirrors the log to `pspsh` and keeps the engine's cycle counters, so every frame
can be broken down on the hardware. `tools/psp/README.md` describes the scripts used for that:
running the PRX from the host over USB, pushing a build to the card, pulling a save game off it to
replay a problem, and the command-line switches that load a save and walk forward, checksum the
frame, or quit on a timer.

## Game data

`fetch-assets.sh` takes the maps, textures, sounds and music from the Unreal Gold disc image and
the code packages from the 1998 *Unreal Special Edition* demo. Unreal Gold's own `System/*.u` are
the v226 code (package file version 68) and the v61 linker of this engine cannot read them; the
demo's Core.u, Engine.u, Fire.u and UnrealI.u are byte-identical to the retail v200 packages, and
Gold's media files are byte-identical to v200's. The header of the script has the sources and
checksums. `install-psp.sh` takes `UNREAL_ASSETS=<dir>` to install another data set and
`UNREAL_DIRNAME=<name>` to install under another folder name; the game finds its data next to its
EBOOT, so two installs can sit side by side.

## The emulator

PPSSPP implements the Media Engine's stock firmware services (the video and audio decoders retail
games use) but not the ME as a second CPU running custom code, which is how this port mixes sound
and plays music. With `MusicME=1` the bridge module's first kernel call fails in the emulator and
the game waits for the ME forever; `MusicME=0` in the `[PSP]` section of `System/Unreal.ini` mixes
on the main CPU instead. Leave it at the default on a real PSP, where that mixing costs frame rate.
PPSSPP pull request [#21554](https://github.com/hrydgard/ppsspp/pull/21554) adds a low-level
emulation of the ME for homebrew; once it ships, the option can stay on. The emulator is several
times faster than a PSP and says nothing about frame rate; it is for logic and memory.

## Related projects

* [fgsfdsfgs/UE1](https://github.com/fgsfdsfgs/UE1) is the upstream of this fork: the SDL2, OpenGL
  and OpenAL drivers, GCC support and the Windows, Linux and **PSVita** ports (by fgsfds, BSzili and
  Rinnegatamante) all come from there. The PSVita port is the closest relative of the PSP one; its
  instructions are kept below.
* [RedPandaProjects/UnrealEngine](https://github.com/RedPandaProjects/UnrealEngine) maintains the
  same 1998 v200 source on GitHub for Windows, with the aim of fixing bugs and improving the code
  while keeping the vanilla game intact. Useful as a readable reference for the original engine.
* [OldUnreal](https://www.oldunreal.com) maintains the modern patches for Unreal Gold and hosts the
  disc image that `fetch-assets.sh` downloads.

## Other platforms (from upstream)

The changes upstream made to the original source apply to every platform: an SDL2 windowing and
input driver (NSDLDrv), GLES2 and fixed-pipeline OpenGL renderers (NOpenGLESDrv and NOpenGLDrv), an
OpenAL plus libxmp audio driver (NOpenALDrv), GCC support and many related bug fixes. The editor UI
is not supported. These builds need the original retail v200 release of Unreal or the v205 demo.

### Running on Linux and Windows
1. Install the original retail v200 release of Unreal or the v205 demo.
2. Copy over the new files:
   * If you downloaded a ZIP from the Releases section:
     1. Unzip said ZIP to the `Unreal` folder. Overwrite everything.
   * If you built the game yourself:
     1. Copy the .dll/.so/.exe/.bin files you built to `Unreal/System`. Overwrite everything.
     2. Copy the contents of `Engine/Config` to `Unreal/System`. Overwrite everything.
3. Run `System/Unreal.exe`.

### Running on the PSVita
1. Ensure you have libshacccg installed.
2. Install the original retail v200 release of Unreal or the v205 demo onto your PC.
3. Copy the contents of the `Unreal` folder to `ux0:/data/unreal/` on your PSVita.
4. Copy the `unreal` folder from `unreal-arm-psvita-gcc.zip` to `ux0:/data/`. Overwrite everything.
5. Install `unreal.vpk` from `unreal-arm-psvita-gcc.zip`.
6. Run Unreal.

### Building for Windows x86 (MSYS2/MinGW)
1. Install MSYS2.
2. Open the `MINGW32` prompt. **Do not** use the `MINGW64` or `MSYS` prompts.
3. Install dependencies: `pacman -S git make mingw-w64-i686-toolchain mingw-w64-i686-cmake mingw-w64-i686-SDL2 mingw-w64-i686-openal mingw-w64-i686-libxmp`
4. Build:
   ```
   cmake -G"Unix Makefiles" -Bbuild Source
   cmake --build build -j4 -- -O && cmake --install build
   ```
5. The resulting files will be in `build/RelWithDebInfo` by default.

### Building for Windows x86 (Visual Studio)
1. Install VS2019 or VS2022. Dependencies are included in the repo.
2. Build:
   ```
   cmake -Bbuild -G"Visual Studio 16 2019" -A Win32 Source # or -G"Visual Studio 17 2022"
   cmake --build build && cmake --install build --config RelWithDebInfo
   ```
3. The resulting files will be in `build/RelWithDebInfo` by default.

### Building for Linux x86
1. Install git, make, cmake, gcc, g++, sdl2, libopenal, libxmp.
   * If cross-compiling from x86_64, also install 32-bit versions of the libraries and gcc-multilib/g++-multilib.
   * On Debian x86_64 this process looks something like this:
     ```
     sudo dpkg --add-architecture i386
     sudo apt-get -y update
     sudo apt-get -y install git gcc g++ gcc-multilib g++-multilib make cmake
     sudo apt-get -y install libsdl2-dev libopenal-dev libxmp-dev libsdl2-dev:i386 libopenal-dev:i386 libxmp-dev:i386
     ```
2. Build:
   ```
   cmake -G"Unix Makefiles" -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -Bbuild Source # if on x86_64
   cmake -G"Unix Makefiles" -Bbuild Source # if on i686
   cmake --build build -j4 -- -O && cmake --install build
   ```
3. The resulting files will be in `build/RelWithDebInfo` by default.

### Building for Linux ARM
1. Install git, make, cmake, gcc, g++, sdl2, libopenal, libxmp.
   * If cross-compiling from ARM64, also install armhf versions of the libraries and arm-linux-gnueabihf-gcc/g++.
   * On Debian x86_64 or ARM64 this process looks something like this:
     ```
     sudo dpkg --add-architecture armhf
     sudo apt-get -y update
     sudo apt-get -y install git gcc g++ crossbuild-essential-armhf make cmake
     sudo apt-get -y install libsdl2-dev libopenal-dev libxmp-dev libsdl2-dev:armhf libopenal-dev:armhf libxmp-dev:armhf
     ```
2. Build:
   ```
   cmake -G"Unix Makefiles" -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc-12 -DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++-12 -Bbuild Source
   cmake --build build -j4 -- -O && cmake --install build
   ```
3. The resulting files will be in `build/RelWithDebInfo` by default.

### Building for the PSVita (on Linux or WSL)
1. Install VitaSDK with all VDPM packages and ensure the `VITASDK` environment variable is set and `$VITASDK/bin` is in your `PATH`.
2. Build and install vitaGL:
   ```
   git clone --recursive https://github.com/Rinnegatamante/vitaGL
   make -C vitaGL HAVE_GLSL_SUPPORT=1 CIRCULAR_VERTEX_POOL=2 MATH_SPEEDHACK=1 INDICES_SPEEDHACK=1 PRIMITIVES_SPEEDHACK=1 -j install
   ```
3. Build and install SDL2:
   ```
   git clone --recursive --branch vitagl https://github.com/Northfear/SDL
   pushd SDL
   cmake -S. -Bbuild -DCMAKE_TOOLCHAIN_FILE=${VITASDK}/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DVIDEO_VITA_VGL=ON
   cmake --build build -- -j
   cmake --install build
   popd
   ```
4. Build the VPK:
   ```
   cmake -G"Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="${VITASDK}/share/vita.toolchain.cmake" -Bbuild -DCMAKE_BUILD_TYPE=RelWithDebInfo Source
   cmake --build build -j
   ```
5. The VPK will be in `build/Unreal/`.
