#!/usr/bin/env bash
# Assembles Xenia's PowerPC instruction tests for the Nintendo Switch harness.
#
# Equivalent to `xb gentests` (same binutils invocations), but writes only what
# the console needs into one flat folder:
#
#   <out>/<suite>.bin   test code, linked at 0x80000000
#   <out>/<suite>.map   `nm` symbol map (test_* entry points)
#   <out>/<suite>.s     only the `test_*:` labels and `#_` annotations
#   <out>/skip.txt      test cases to skip
#
# Usage: tools/switch/gen-ppc-tests.sh <binutils-bin-dir> <out-dir>
#   <binutils-bin-dir> contains powerpc-none-elf-{as,ld,nm}; build it with
#   third_party/binutils/build.sh (Xenia's binutils 2.24 + VMX128 patch).

set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <binutils-bin-dir> <out-dir>" >&2
  exit 2
fi

BIN="$(cd "$1" && pwd)"
OUT="$2"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/src/xenia/cpu/ppc/testing"

AS="$BIN/powerpc-none-elf-as"
LD="$BIN/powerpc-none-elf-ld"
NM="$BIN/powerpc-none-elf-nm"
for tool in "$AS" "$LD" "$NM"; do
  [ -x "$tool" ] || { echo "missing $tool" >&2; exit 1; }
done

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

assemble_one() {
  local src="$1"
  local name
  name="$(basename "$src" .s)"

  "$AS" -a32 -be -mregnames -ma2 -maltivec -mvsx -mvmx128 -R \
    -o"$TMP/$name.o" "$src"
  # "-A powerpc:common32" is intentionally a single argument, exactly as
  # `xb gentests` passes it.
  "$LD" "-A powerpc:common32" -melf32ppc -EB -nostdlib --oformat=binary \
    -Ttext=0x80000000 -e0x80000000 -o"$OUT/$name.bin" "$TMP/$name.o"
  "$NM" --numeric-sort "$TMP/$name.o" > "$OUT/$name.map"

  grep -E '^[[:space:]]*(test_[A-Za-z0-9_]+:|#_ )' "$src" > "$OUT/$name.s" || true
}
export -f assemble_one
export AS LD NM TMP OUT

fail=0
ls "$SRC"/instr_*.s | xargs -P "$(nproc)" -I{} bash -c 'assemble_one "$1"' _ {} \
  || fail=1

cp "$SRC/skip.txt" "$OUT/skip.txt"

count=$(ls "$OUT"/instr_*.bin 2>/dev/null | wc -l)
total=$(ls "$SRC"/instr_*.s | wc -l)
echo "Assembled $count of $total test files into $OUT"
[ "$fail" -eq 0 ] && [ "$count" -eq "$total" ]
