#!/usr/bin/env bash
# reEye: a crash.txt or heapcheck.txt (written on the console) with its call stacks as function names and source
# lines; every other line is kept as it is.
#   tools/symbolize-crash.sh /path/to/crash.txt [build-ps5/link/llvm-pie.elf]
# The program loads somewhere else than the link put it: the base is where main is now (the file's "main at")
# minus main's address in the linked program. Every problem is printed on standard output, so it ends up in the
# file the output is saved to.
crash=${1:-}
elf=${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build-ps5/link/llvm-pie.elf}
fail() { echo "symbolize-crash: $*"; exit 1; }
[ -n "$crash" ] || fail "usage: tools/symbolize-crash.sh crash.txt [llvm-pie.elf]"
[ -s "$crash" ] || fail "no crash file at $crash"
[ -s "$elf" ] || fail "no linked program at $elf (run ./build-ps5.sh first)"
for tool in llvm-nm-18 llvm-symbolizer-18; do
    command -v "$tool" > /dev/null || fail "$tool is missing: sudo apt install llvm-18"
done
main_now=$(sed -nE 's/^main at (0x)?([0-9a-fA-F]+).*/\2/p' "$crash" | sed -n 1p)
# the whole symbol list is read (no early exit), so llvm-nm never stops on a closed pipe
main_link=$(llvm-nm-18 "$elf" 2>&1 | awk '$3 == "main" && !found { print $1; found = 1 }')
[ -n "$main_now" ] || fail "no 'main at' line in $crash"
[ -n "$main_link" ] || fail "no main in $elf"
base=$(( 0x$main_now - 0x$main_link ))
# the build that crashed and the one here must be the same, or every name below is wrong
crash_build=$(sed -n 's/^build //p' "$crash" | sed -n 1p)
elf_build=$(cat "$(dirname "$elf")/build_id.txt" 2>/dev/null || true)
if [ -n "$crash_build" ] && [ -n "$elf_build" ] && [ "$crash_build" != "$elf_build" ]; then
    fail "this crash is from build $crash_build, but the program here is build $elf_build: the names would be wrong"
fi
if [ -z "$crash_build" ] || [ "$base" != $((0x400000)) ]; then
    echo "symbolize-crash: WARNING: $( [ -z "$crash_build" ] && echo "the crash file names no build (an older one)" || echo "load base is not 0x400000" ): it is probably from another build, and the names below would be wrong"
fi
printf 'program %s\nmain at 0x%s, linked at 0x%s: load base 0x%x\n' "$elf" "$main_now" "$main_link" "$base"
tr -d '\r' < "$crash" | while IFS= read -r line; do
    if [[ $line =~ ^[[:space:]]*#([0-9]+)[[:space:]]+0x([0-9a-fA-F]+) ]]; then
        # return addresses: the call is the instruction before
        off=$(( 0x${BASH_REMATCH[2]} - base - 1 ))
        printf '  #%s 0x%x  ' "${BASH_REMATCH[1]}" "$off"
        llvm-symbolizer-18 --obj="$elf" -C -f --inlines "0x$(printf '%x' "$off")" 2>&1 | sed '/^$/d' | paste -sd' ' -
    elif [[ $line != "main at "* ]]; then
        printf '%s\n' "$line"
    fi
done
