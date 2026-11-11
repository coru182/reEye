#!/usr/bin/env bash
# reEye setup: the pinned dependencies, then the game's code recompiled from your own default.xex.
#
#   ./setup.sh /path/to/xbla-files        a folder holding the game's extracted XBLA files (default.xex and the rest)
#   ./setup.sh --toolchain                only the dependencies and the PS5 toolchain (no game files needed)
#
# deps/        GoldenEye-Recomp (the PC port, SunJaycy), its ReXGlue SDK fork with submodules, and reUndr-ps5 with its
#              PS5 toolchain (PayloadSDK, RADV built for the PS5, the packaging tool), as reKing pins it
# work/rexglue the ReXGlue CLI built for this Linux machine (it runs the recompiler)
# work/game/   GoldenEye-Recomp's sources with patches/ applied, your default.xex in assets/, and generated/: the C++
#              the recompiler makes from it
# Run it again with another default.xex to recompile; what was downloaded and built is kept.
# REUNDR=/path/to/reUndr-ps5 uses a reUndr-ps5 checkout whose toolchain is already built (that build takes an hour
# or more and needs Docker); a reKing checkout's deps/reUndr-ps5 is one.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEPS=$ROOT/deps
SRC=${1:-}
if [ "$SRC" != --toolchain ]; then
    if [ ! -f "$SRC/default.xex" ]; then
        echo "usage: ./setup.sh /path/to/xbla-files   (the folder with default.xex, from your GoldenEye 007 XBLA package)"
        echo "       ./setup.sh --toolchain           (the dependencies and the PS5 toolchain only)"
        exit 2
    fi
    SRC=$(cd "$SRC" && pwd)
    [ "$(head -c 4 "$SRC/default.xex")" = XEX2 ] || { echo "setup: $SRC/default.xex is not an Xbox 360 XEX2 file"; exit 2; }
fi
JOBS=${JOBS:-$(nproc)}
export PATH=/usr/lib/llvm-18/bin:$PATH
for tool in git cmake ninja clang-18 clang++-18 ld.lld llvm-objcopy-18 llvm-objdump-18 python3 patch zip; do
    command -v "$tool" >/dev/null || { echo "setup: $tool is missing (README.md, Building)"; exit 1; }
done

# a repository at a pinned commit: name, url, commit
pin() {
    local dir=$DEPS/$1
    [ -d "$dir/.git" ] || git clone --filter=blob:none --no-checkout "$2" "$dir"
    git -C "$dir" checkout --quiet --detach "$3" 2>/dev/null ||
        { git -C "$dir" fetch --quiet origin && git -C "$dir" checkout --quiet --detach "$3"; }
}
mkdir -p "$DEPS"
pin GoldenEye-Recomp https://github.com/SunJaycy/GoldenEye-Recomp.git fdee4d1f750aff4c3b5c6ba3d60f20281c21447d
pin rexglue https://github.com/SunJaycy/GoldenEye-Recomp-rexglue.git 2824a80ecaa541084e1e1fa3e16c77b0d2f6d9e4 # 0.8.0
# the SDK's submodules, all but the profiler (Tracy) and the tests (Catch2); its CMake always builds SDL3, which the
# PS5 build keeps until ps5/ replaces its input and sound (docs/PORTING.md)
SUBS=(libmspack glslang FFmpeg tomlplusplus simde xxHash spdlog fmt snappy utfcpp volk vulkan-headers
      vulkan-memory-allocator imgui spirv-tools spirv-headers cli11 o1heap inja sdl3)
git -C "$DEPS/rexglue" submodule update --init --depth 1 --jobs "$JOBS" -- "${SUBS[@]/#/thirdparty/}"
# this port's changes to the SDK, on a clean checkout each time (patches/rexglue-*.patch, docs/PORTING.md)
git -C "$DEPS/rexglue" checkout --quiet -- .
for p in "$ROOT"/patches/rexglue-*.patch; do [ -s "$p" ] && git -C "$DEPS/rexglue" apply "$p"; done

# reUndr-ps5's pinned PS5 toolchain, built by its own scripts (the same pin and steps as reKing's setup.sh)
REUNDR=${REUNDR:-$DEPS/reUndr-ps5}
[ "$REUNDR" != "$DEPS/reUndr-ps5" ] ||
    pin reUndr-ps5 https://github.com/bystritskiy/reUndr-ps5.git 91de6198be34c956069e9b5d9ce00aed532a73ab
VK=$REUNDR/build/deps/PS5_Vulkan
if [ ! -f "$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" ] || [ ! -f "$VK/runtime/libc.prx" ] ||
   [ ! -f "$REUNDR/build/deps/compiler-rt/libclang_rt.builtins-x86_64.a" ]; then
    command -v docker >/dev/null || { echo "setup: building reUndr-ps5's toolchain needs Docker"; exit 1; }
    (cd "$REUNDR" && python3 tools/rebuild-deps.py --llvm-bin /usr/lib/llvm-18/bin --jobs "$JOBS")
fi
if [ ! -x "$REUNDR/build/ps5-native-tool" ]; then
    mkdir -p "$REUNDR/build"
    sed "s#/opt/homebrew/opt/llvm@18/bin/clang++#$(command -v clang++-18)#" "$REUNDR/tools/build-native-tool.py" \
        > "$REUNDR/build/build-native-tool-linux.py"
    (cd "$REUNDR" && python3 build/build-native-tool-linux.py)
fi
if [ "$SRC" = --toolchain ]; then
    echo "setup: the toolchain is ready"
    exit 0
fi

# the ReXGlue CLI for this machine: it runs the recompiler (only the CLI target, no game, no graphics)
T=$ROOT/work/rexglue
if [ ! -x "$T/rexglue" ]; then
    cmake -S "$DEPS/rexglue" -B "$T/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-18 \
        -DCMAKE_CXX_COMPILER=clang++-18 -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF > /dev/null
    cmake --build "$T/build" --target rexglue -j "$JOBS"
    cli=$(find "$T/build" "$DEPS/rexglue/out" -type f -name rexglue -perm -u+x 2>/dev/null | head -1)
    [ -n "$cli" ] || { echo "setup: the rexglue CLI was not built"; exit 1; }
    cp "$cli" "$T/rexglue"
fi

# the game's tree: GoldenEye-Recomp's sources, with this port's changes, and your default.xex
G=$ROOT/work/game
rm -rf "$G"
mkdir -p "$G/assets"
cp -r "$DEPS/GoldenEye-Recomp/." "$G/"
rm -rf "$G/.git"
for p in "$ROOT"/patches/goldeneye-*.patch; do [ -s "$p" ] && patch -s -d "$G" -p1 < "$p"; done
cp "$SRC/default.xex" "$G/assets/default.xex"
(cd "$G" && "$T/rexglue" codegen ge_manifest.toml)
[ -d "$G/generated" ] || { echo "setup: the recompiler made no generated/ folder"; exit 1; }
echo "setup: $(ls "$G"/generated/*.cpp | wc -l) generated sources; now ./build-ps5.sh"
