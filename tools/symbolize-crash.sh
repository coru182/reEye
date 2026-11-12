#!/usr/bin/env bash
# reEye: the call stack of a crash.txt (written by abort() on the console) as function names and source lines.
#   tools/symbolize-crash.sh /path/to/crash.txt [build-ps5/link/llvm-pie.elf]
# The program loads somewhere else than the link put it: the base is where main is now (crash.txt's "main at")
# minus main's address in the linked program.
set -euo pipefail
crash=${1:?usage: tools/symbolize-crash.sh crash.txt [llvm-pie.elf]}
elf=${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build-ps5/link/llvm-pie.elf}
main_now=$(sed -nE 's/^main at (0x)?([0-9a-fA-F]+).*/\2/p' "$crash" | head -1)
main_link=$(llvm-nm-18 "$elf" | awk '$3 == "main" { print $1; exit }')
[ -n "$main_now" ] && [ -n "$main_link" ] || { echo "no main address in $crash or $elf"; exit 1; }
base=$(( 0x$main_now - 0x$main_link ))
printf 'load base 0x%x\n' "$base"
grep -oE '#[0-9]+ 0x[0-9a-fA-F]+' "$crash" | while read -r frame addr; do
    # return addresses: the call is the instruction before
    off=$(( addr - base - 1 ))
    printf '%s 0x%x  ' "$frame" "$off"
    llvm-symbolizer-18 --obj="$elf" -C -f --inlines "0x$(printf '%x' "$off")" | paste -sd' ' | sed 's/  */ /g'
done
