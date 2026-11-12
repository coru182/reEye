# reEye

**GoldenEye 007, the cancelled Xbox 360 / XBLA version, statically recompiled into a native app for
homebrew-enabled PS5 consoles.**

> [!WARNING]
> **Work in progress: nothing to play yet.** The build pipeline is in place; the PS5 runtime is being ported
> ([docs/PORTING.md](docs/PORTING.md)).

> [!IMPORTANT]
> **Bring your own files.** The app contains none of the game's files: no XEX, textures, audio or generated code.
> Please don't ask for or share game files here.

## What it is

The Xbox 360 game's code is translated into C++ ahead of time and compiled for the PS5, as
[reKing](https://github.com/coru182/reKing) does for The Return of the King. It's not an emulator. It builds on
[GoldenEye-Recomp](https://github.com/SunJaycy/GoldenEye-Recomp) by SunJaycy, the PC port made with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), and on reKing's PS5 pieces: RADV (Mesa's Vulkan driver)
built for the PS5 by reUndr-ps5's toolchain, presented on VideoOut.

## Requirements (once it runs)

- A PS5 that runs homebrew, with [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) and an FTP server.
- The game's extracted XBLA files (`default.xex` and the rest of the package's contents).

## Install (once it runs)

1. Copy the `PPSA99070` folder from the release zip to `/data/homebrew/` on the console. The **reEye** tile appears
   after about 30 seconds.
2. Copy the game's extracted files to `/data/reeye/game/`.
3. Start the tile.

## For developers

### Building

You need a Linux machine (Ubuntu 24.04; WSL 2 should do) with:

```sh
sudo apt install git cmake ninja-build clang-18 lld-18 llvm-18 llvm-18-dev zlib1g-dev python3 python3-pip patch zip \
    pkg-config libgtk-3-dev libx11-xcb-dev clang-20
```

and Docker, for reUndr-ps5's PS5 toolchain.

```sh
git clone https://github.com/coru182/reEye.git
cd reEye
./setup.sh /path/to/xbla-files      # dependencies, the ReXGlue CLI, then the game's code recompiled from default.xex
./build-ps5.sh                      # the app: build-ps5/PPSA99070-<version>.zip
```

`setup.sh` uses the same pinned PS5 toolchain as reKing. If reKing is already built on the machine, reuse its
toolchain instead of building it again (about an hour):

```sh
REUNDR=/path/to/reKing/deps/reUndr-ps5 ./setup.sh /path/to/xbla-files
REUNDR=/path/to/reKing/deps/reUndr-ps5 ./build-ps5.sh
```

While the port is under way, `./build-ps5.sh compile` stops after compiling and writes
`build-ps5/compile-errors.txt`: what still has to change for the PS5.

### When it crashes on the console

The app writes what it knows to `/data/reeye` before it ends:

- `log.txt`: the runtime's log (written out every second).
- `crash.txt`: on an abort, the call stack as addresses, and the heap's state.
- `heapcheck.txt`: with the heap checks built in (`HEAPCHECK=1`, the default for now), the first damage found in
  the heap: which block, how big, the call stack that allocated it (and freed it), the bytes found, and the call
  stack that found it (`ps5/heapcheck_ps5.c`). `HEAPCHECK=0 ./build-ps5.sh` leaves the checks out.

Copy them to the build machine and turn their call stacks into function names with the program that ran (the one in
`build-ps5/link`, so rebuild nothing in between):

```sh
tools/symbolize-crash.sh crash.txt
tools/symbolize-crash.sh heapcheck.txt
```

### How it fits together

- [GoldenEye-Recomp](https://github.com/SunJaycy/GoldenEye-Recomp) (SunJaycy, The Unlicense): the game's hooks,
  fixes, menus and the recompiler's configuration.
- [GoldenEye-Recomp-rexglue](https://github.com/SunJaycy/GoldenEye-Recomp-rexglue): the ReXGlue SDK fork it builds
  against (BSD-3-Clause, derived from Xenia): the recompiler and the Xbox 360 runtime (kernel, Xenos on Vulkan, XMA).
- `patches/`: this port's changes to them (`rexglue-ps5.patch`, `goldeneye-ps5.patch`).
- `ps5/`: the PS5 host, from reKing.
- reUndr-ps5's toolchain (PayloadSDK, RADV for the PS5, the packaging tool), as reKing pins it.

## Licence

GPL-3.0 (`LICENSE`), like reKing; the parts it builds on keep their own licences (`package/licenses/`).
