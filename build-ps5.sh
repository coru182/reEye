#!/usr/bin/env bash
# reEye: builds the PS5 app (PPSA99070) into build-ps5/PPSA99070-<version>.zip, after ./setup.sh, from
#   work/game      GoldenEye-Recomp's sources (with patches/) and generated/, the C++ the recompiler made from your
#                  own default.xex; it builds against deps/rexglue, the ReXGlue SDK fork, with the PS5 changes
#   ps5/           the PS5 host: entry, the POSIX calls a PS5 app may not make itself
#   package/       what the app carries besides the program: sce_sys (param.json, icon), licences, README.txt
#   art/           optional: your own icon0.png, pic0.dds, pic1.dds, in place of package/sce_sys's
#   deps/          reUndr-ps5's PS5 toolchain (PayloadSDK, RADV, the packaging tool), as reKing uses it
# Stages: ./build-ps5.sh [compile] [link] [package]  (all three when none is named). JOBS=<n> compiles n at a time.
#
# STATUS: work in progress. The compile stage builds ReXGlue and the game with the PS5 compiler and keeps going past
# errors, so build-ps5/compile-errors.txt lists every source the port still has to change (docs/PORTING.md).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEPS=$ROOT/deps
GAME=$ROOT/work/game
PKG=$ROOT/package
REUNDR=${REUNDR:-$DEPS/reUndr-ps5}
VK=$REUNDR/build/deps/PS5_Vulkan
SDK=$VK/.deps/native/ps5-payload-sdk
OUT=$ROOT/build-ps5
TITLE=PPSA99070
VERSION=0.0.1
JOBS=${JOBS:-$(nproc)}
export PATH=/usr/lib/llvm-18/bin:$PATH
STAGES=("$@"); [ ${#STAGES[@]} -gt 0 ] || STAGES=(compile link package)
has() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
[ -d "$GAME/generated" ] || { echo "build: run ./setup.sh /path/to/xbla-files first"; exit 1; }
[ -d "$SDK/target/include" ] || { echo "build: the PS5 toolchain is missing; run ./setup.sh --toolchain"; exit 1; }
mkdir -p "$OUT/host"

if has compile; then
    # the game and ReXGlue through their own CMake, with reUndr's PS5 toolchain file: Vulkan only (RADV, linked in
    # whole later), no SDL, no Tracy, no tests. REX_PLATFORM_PS5 marks the code paths the port changes.
    B=$OUT/cmake
    cmake -S "$GAME" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$REUNDR/ps5/cmake/ps5.cmake" \
        -DPS5_PAYLOAD_SDK="$SDK" -DCMAKE_C_COMPILER=/usr/lib/llvm-18/bin/clang \
        -DCMAKE_CXX_COMPILER=/usr/lib/llvm-18/bin/clang++ -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR="$DEPS/rexglue" \
        -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF \
        -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
        -DCMAKE_C_FLAGS="-DREX_PLATFORM_PS5=1 -ffunction-sections -fdata-sections" \
        -DCMAKE_CXX_FLAGS="-DREX_PLATFORM_PS5=1 -ffunction-sections -fdata-sections" > "$OUT/configure.log" 2>&1 ||
        { tail -30 "$OUT/configure.log"; echo "compile: CMake configure failed (see $OUT/configure.log)"; exit 1; }
    # -k 0: keep going past failing sources; the PC executable's own link is not used (the PS5 link is below)
    set +e
    cmake --build "$B" -j "$JOBS" -- -k 0 > "$OUT/compile.log" 2>&1
    set -e
    grep -E '^FAILED: ' "$OUT/compile.log" | sed -E 's/^FAILED: //' | grep -v -E '/GoldenEye$|/ge$' | sort -u \
        > "$OUT/compile-errors.txt" || true
    for f in "$ROOT"/ps5/*.c; do
        o=$OUT/host/$(basename "$f" .c).o
        [ "$o" -nt "$f" ] || "$SDK/bin/prospero-clang" -std=c11 -O2 -ffunction-sections -fdata-sections -fno-plt \
            -I"$ROOT" -c "$f" -o "$o" || exit 1
    done
    if [ -s "$OUT/compile-errors.txt" ]; then
        echo "compile: $(wc -l < "$OUT/compile-errors.txt") targets still fail on the PS5 (see $OUT/compile-errors.txt"
        echo "         and the errors in $OUT/compile.log)"
        exit 1
    fi
    echo "compile: done"
fi

if has link; then
    # reKing's link (reUndr's tools/link.sh, MIT; the RADV recipe and native runtime are GPL-3.0-or-later): RADV
    # linked in whole, the payload SDK's libc++/libc++abi/libunwind, its system module stubs, one export map
    B=$OUT/cmake
    work=$OUT/link; mkdir -p "$work/obj" "$work/stubs"
    archive=$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
    resource=$(clang-18 --print-resource-dir); rt="$REUNDR/build/deps/compiler-rt/libclang_rt.builtins-x86_64.a"
    if ! cmp -s "$rt" "$resource/lib/linux/libclang_rt.builtins-x86_64.a"; then
        { mkdir -p "$resource/lib/linux" && cp "$rt" "$resource/lib/linux/"; } 2>/dev/null ||
            sudo sh -c "mkdir -p '$resource/lib/linux' && cp '$rt' '$resource/lib/linux/'"
    fi
    for pair in 'libSceAgc agc_canary_link_stub.c' 'libSceAgcDriver agc_driver_canary_link_stub.c'; do
        read -r library source <<<"$pair"
        "$SDK/bin/prospero-clang" -std=c11 -O2 -fPIC -c "$VK/vendor/ps5/sdk/stubs/$source" -o "$work/obj/$library.o"
        "$SDK/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/stubs/$library.so" "$work/obj/$library.o"
    done
    export LLVM_CONFIG=/usr/bin/llvm-config-18 PS5_CLANG=/usr/bin/clang-18
    source "$VK/tools/radv-link.sh"
    radv_link_recipe "$VK" "$SDK" "$archive"
    inputs=(); for item in "${radv_link_inputs[@]}"; do case "$item" in --start-group|--end-group) ;; *) inputs+=("$item") ;; esac; done
    flags=(); skip=0
    for item in "${radv_link_flags[@]}"; do
        if ((skip)); then skip=0; continue; fi
        if [[ $item == --version-script ]]; then skip=1; continue; fi
        flags+=("$item")
    done
    # libc.a's raw-syscall readlink, pathconf and fork share an object the link needs: bound to ps5/posix_ps5.c's
    for name in readlink pathconf fork; do flags+=("--defsym=$name=reeye_$name"); done
    { printf '{local:\n_Zn*; _Zd*;\n'
      for item in "${flags[@]}"; do [[ $item == --defsym=* ]] && { n=${item#--defsym=}; printf '%s;\n' "${n%%=*}"; }; done
      printf '*;};\n'; } > "$work/exports.map"
    # the game's objects and every static library CMake built (ReXGlue's modules and its third-party code)
    mapfile -t game < <(find "$B/CMakeFiles" -path '*/ge.dir/*' -name '*.o')
    mapfile -t libs < <(find "$B" -name '*.a')
    stubs=(); for f in "$SDK"/target/lib/*.so; do case "$f" in *libScePosixForWebKit.so|*libSceGLSlimVSH.so|*libc_stub_weak.so) ;; *) stubs+=("$f") ;; esac; done
    "$SDK/bin/prospero-lld" "${radv_linker_script[@]}" "${flags[@]}" --version-script "$work/exports.map" \
        --exclude-libs=ALL --no-dynamic-linker --gc-sections -e _start -o "$work/llvm-pie.elf" \
        "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
        --start-group "$OUT"/host/*.o "${game[@]}" "${libs[@]}" "${inputs[@]}" \
        "$SDK/target/lib/libc.a" "$SDK/target/lib/libpthread.a" --end-group --as-needed "${stubs[@]}" > "$work/link.log" 2>&1 || true
    grep -E "error|undefined" "$work/link.log" | head -40 || true
    [ -s "$work/llvm-pie.elf" ] && [ "$work/llvm-pie.elf" -nt "$work/exports.map" ] || { echo "link: failed (see $work/link.log)"; exit 1; }
    # a PS5 title may not issue a syscall itself (SYSTEM_ILLEGAL_FUNCTION_CALL): none may be left in the program
    llvm-objdump-18 -d --no-show-raw-insn "$work/llvm-pie.elf" | awk '/^[0-9a-f]+ <.*>:$/ {fn=$2} /\tsyscall/ {print fn}' \
        | sort | uniq -c > "$work/raw_syscalls.txt"
    if [ -s "$work/raw_syscalls.txt" ]; then echo "link: raw syscalls left in:"; cat "$work/raw_syscalls.txt"; exit 1; fi
    echo "link: no raw syscalls"
    echo "link: $(du -h "$work/llvm-pie.elf" | cut -f1) $work/llvm-pie.elf"
fi

if has package; then
    # reUndr's tools/package.py steps: module info and imports, a signed fake SELF, the PS5 libc module, sce_sys
    work=$OUT/link; native=$REUNDR/build/ps5-native-tool; app=$OUT/app/$TITLE
    [ -x "$native" ] || { echo "package: the native tool is not built ($native)"; exit 1; }
    rm -rf "$app"; mkdir -p "$app/sce_module"
    llvm-objcopy-18 --strip-debug "$work/llvm-pie.elf" "$work/llvm-pie.stripped.elf"
    "$native" link --in "$work/llvm-pie.stripped.elf" --out "$work/eboot.elf" --stub-dir "$SDK/target/lib" \
        --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
        --companion-sdk 0x08050001 --file-name eboot.elf > "$work/convert.log" 2>&1
    "$native" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$work/sign.log" 2>&1
    cp "$VK/runtime/libc.prx" "$app/sce_module/libc.prx"
    for f in "$app/eboot.bin" "$app/sce_module/libc.prx"; do
        "$native" self --inspect --file "$f" | grep -q "integrity: valid" || { echo "package: $f is not a valid fake SELF"; exit 1; }
    done
    cp -r "$PKG/sce_sys" "$app/sce_sys"
    for f in icon0.png pic0.dds pic1.dds; do if [ -f "$ROOT/art/$f" ]; then cp "$ROOT/art/$f" "$app/sce_sys/$f"; fi; done
    mkdir -p "$app/licenses" && cp "$PKG"/licenses/* "$app/licenses/"
    cp "$PKG/README.txt" "$app/README.txt"
    (cd "$OUT/app" && rm -f "../$TITLE-$VERSION.zip" && zip -qr "../$TITLE-$VERSION.zip" "$TITLE")
    echo "package: $OUT/$TITLE-$VERSION.zip"
fi
