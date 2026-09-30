## What?

Unreal Engine 1 v200 source with modifications to make it run on modern systems.  
Requires assets from the original Unreal v200 retail release or from the v205 demo. Other versions have not been tested. For the PSP port, `fetch-assets.sh` assembles a compatible set from freely downloadable archives (see below).

## Changes from original source

* Added SDL2 windowing/client driver (NSDLDrv).
* Added GLES2 and fixed pipeline GL graphics drivers (NOpenGLESDrv and NOpenGLDrv).
* Added OpenAL + libxmp audio driver (NOpenALDrv).
* Added GCC support and fixed a bunch of related bugs.
* Supported platforms: Windows (x86), Linux (x86, ARM32), PSVita (ARM32) and **PSP** (MIPS, this fork).
* Editor UI is not supported.

## PSP port

This fork adds a port to the Sony PSP. It runs the full single-player game on a PSP-2000 or later
(64 MB of RAM is required; the PSP-1000's 32 MB is not enough) with custom firmware that can run
homebrew EBOOTs (PRO, ME, ARK). It is a work in progress: the first levels are playable at roughly
20-30 fps, level loads take 8-20 seconds from the Memory Stick, and `PSP-TODO.md` is the running
engineering log of what was done and what is still open. Controls are described in
`PSP-CONTROLS.txt`.

What the port does differently from the other platforms, in short: textures, sounds and mesh data
are streamed from the Memory Stick on first use instead of being kept in memory; sound effects are
mixed by the PSP's second CPU, the Media Engine, which also plays the tracker music; BSP lighting is
folded into vertex colours; large allocations live in their own region so level changes do not
fragment the heap. See the comments at the top of `Source/NOpenALDrv/PspMix.h`,
`Source/Core/Src/UnFile.cpp` and `Source/NOpenGLDrv/NOpenGLDrv.cpp` for the details.

### Running on a PSP

1. Get the game data into `GAME_ASSETS/` next to this README. Either:
   - run `./fetch-assets.sh`. It downloads two freely available archives, checks them against known
     checksums and unpacks the right folders: the maps, textures, sounds and music come from the
     Unreal Gold disc image that OldUnreal hosts with Epic Games' permission (you are asked to accept
     the Epic Games Terms of Service, as their installer does), and the code packages in `System/`
     come from the 1998 *Unreal Special Edition* demo preserved on archive.org. Both parts are
     byte-identical to the original v200 release, which is what this engine was built from. Unreal
     Gold's own `System/*.u` are the later v226 code and cannot be loaded by this engine, so they
     are not used. About 780 MB is downloaded and 600 MB kept.
   - or, if you own the original 1998 CD, copy its `System`, `Maps`, `Textures`, `Sounds` and
     `Music` folders into `GAME_ASSETS/` yourself (about 370 MB).

   `GAME_ASSETS/` is ignored by git; never commit it.
2. Build the EBOOT (below) or take `EBOOT.PBP` from a release.
3. Assemble the install. With the Memory Stick mounted:
   ```
   ./install-psp.sh /Volumes/<your stick>/PSP/GAME
   ```
   This creates `PSP/GAME/Unreal/` with the EBOOT, the game data, the port's configuration files
   and the PSP-specific settings appended to `System/Unreal.ini`. The folder may be renamed or moved
   afterwards (the game finds its data next to the EBOOT); `UNREAL_DIRNAME=<name>` installs under
   another name from the start, and `UNREAL_ASSETS=<dir>` takes the data from another folder, so two
   data sets can sit side by side. The same command with
   `~/.config/ppsspp/PSP/GAME` (or wherever your emulator keeps its games) makes a PPSSPP install;
   set `MusicME=0` in the `[PSP]` section of that copy's `System/Unreal.ini`. PPSSPP implements the
   Media Engine's stock firmware services (the video and audio decoders retail games use) but not
   the ME as a second CPU that can run custom code, which is how this port mixes sound and music.
   With the option on, the bridge module's first kernel call fails in the emulator and the game
   waits forever for the ME. `MusicME=0` mixes everything on the main CPU instead; leave it at the
   default on a real PSP, or the mixing costs frame rate there.
4. On the PSP, launch **Unreal** from the Game menu of the XMB.

Saving works from the in-game menu (the save files go to `PSP/GAME/Unreal/Save/`). The game runs at
333 MHz. Suspending the PSP mid-game with the power switch is untested.

### Controls

The PSP has one analog stick, so the layout follows the usual PSP shooter convention: the stick
moves, the face buttons look, the triggers fire. Everything is an ordinary binding in the
`[Engine.Input]` section of `System/Unreal.ini`, so it can be changed; `PSP-CONTROLS.txt` has the
full list, the button numbering and an alternative layout.

| | |
|---|---|
| Analog stick | move forward/back, strafe |
| Triangle / Cross | look up / down |
| Square / Circle | turn left / right |
| R / L | fire / alt-fire |
| D-pad up / down | jump / duck |
| D-pad left / right | previous / next weapon |
| Start | menu (also confirms in menus) |
| Select | translator |
| Select + D-pad up | use the selected inventory item |
| Select + D-pad left / right | previous / next inventory item |

### Building for PSP

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
replay a problem, and the command-line switches that replay a save and walk forward, checksum the
frame, or quit on a timer. The emulator is good for logic and memory but not for speed: it is several
times faster than a PSP and capped at 20 fps here.

## Running

### Linux and Windows
1. Install the original retail v200 release of Unreal or the v205 demo.
2. Copy over the new files:
   * If you downloaded a ZIP from the Releases section:
     1. Unzip said ZIP to the `Unreal` folder. Overwrite everything.
   * If you built the game yourself:
     1. Copy the .dll/.so/.exe/.bin files you built to `Unreal/System`. Overwrite everything.
     2. Copy the contents of `Engine/Config` to `Unreal/System`. Overwrite everything.
3. Run `System/Unreal.exe`.

### PSVita
1. Ensure you have libshacccg installed.
2. Install the original retail v200 release of Unreal or the v205 demo onto your PC.
3. Copy the contents of the `Unreal` folder to `ux0:/data/unreal/` on your PSVita.
4. Copy the `unreal` folder from `unreal-arm-psvita-gcc.zip` to `ux0:/data/`. Overwrite everything.
5. Install `unreal.vpk` from `unreal-arm-psvita-gcc.zip`.
6. Run Unreal.

## Building

### Windows x86 (MSYS2/MinGW)
1. Install MSYS2.
2. Open the `MINGW32` prompt. **Do not** use the `MINGW64` or `MSYS` prompts.
3. Install dependencies: `pacman -S git make mingw-w64-i686-toolchain mingw-w64-i686-cmake mingw-w64-i686-SDL2 mingw-w64-i686-openal mingw-w64-i686-libxmp`
4. Build:
   ```
   cmake -G"Unix Makefiles" -Bbuild Source
   cmake --build build -j4 -- -O && cmake --install build
   ```
5. The resulting files will be in `build/RelWithDebInfo` by default.

### Windows x86 (Visual Studio)
1. Install VS2019 or VS2022. Dependencies are included in the repo.
2. Build:
   ```
   cmake -Bbuild -G"Visual Studio 16 2019" -A Win32 Source # or -G"Visual Studio 17 2022"
   cmake --build build && cmake --install build --config RelWithDebInfo
   ```
3. The resulting files will be in `build/RelWithDebInfo` by default.

### Linux x86
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

### Linux ARM
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

### PSVita (on Linux or WSL)
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

## Note

Unreal Engine, Unreal and any related trademarks or copyrights are owned by Epic Games. This repository is not affiliated with or endorsed by Epic Games. 
This is based on the v200 source available elsewhere on the Internet, with assets and third party proprietary libraries removed. 
Do not use for commercial purposes.
