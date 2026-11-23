#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

if [[ $# != 2 ]]; then
  printf 'Usage: %s SIMULATOR OUTPUT_DIR\n' "${0##*/}" >&2
  exit 2
fi

src=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
sim=$(realpath -e -- "$1")
if [[ ! -f $sim || ! -x $sim ]]; then
  printf 'Not an executable simulator: %s\n' "$sim" >&2
  exit 2
fi
mkdir -p -- "$2"
out=$(cd -- "$2" && pwd -P)
if [[ $out == "$src" ]]; then
  printf 'Choose an output directory separate from the test sources.\n' >&2
  exit 2
fi
toolchain=${CROSS_COMPILE:-riscv64-unknown-elf-}

cd "$out"
unset DIFFTEST_S_BOOM_TERMINAL_MODE DIFFTEST_S_NATIVE_TRACE DIFFTEST_S_SEMANTIC_OUTPUT
for profile in bare sv39; do
  name="sfence_$profile"
  elf="$out/$name.elf"
  log="$out/$name.log"
  "${toolchain}gcc" -march=rv64i_zicsr_zifencei -mabi=lp64 -nostdlib \
    -Wl,--build-id=none,-T,"$src/sfence.ld" -o "$elf" "$src/$name.S"
  "${toolchain}objdump" -d "$elf" > "$name.dis"
  "${toolchain}nm" -P --defined-only "$elf" > "$name.symbols"
  sha256sum "$src/$name.S" "$src/sfence.ld" "$elf" > "$name.sha256"
  terminal=$(awk '$1 == "terminal_fence" {print "0x" $3}' "$name.symbols")
  success=$(awk '$1 == "success" {print "0x" $3}' "$name.symbols")
  : "${terminal:?ELF lacks terminal_fence}" "${success:?ELF lacks success}"

  if ! env DIFFTEST_S_ELF="$elf" DIFFTEST_S_RAM_BASE=0x80000000 DIFFTEST_S_RAM_SIZE=0x10000000 \
    DIFFTEST_S_BOOM_TERMINAL_PC="$terminal" DIFFTEST_S_BOOM_SUCCESS_ADDRESS="$success" \
    DIFFTEST_S_BOOM_SUCCESS_VALUE=1 "$sim" +permissive +max-cycles=20000 +custom_boot_pin=1 \
    +verilator+rand+reset+0 +loadmem="$elf" +difftest_s_boom_terminal_enable=1 \
    "+difftest_s_boom_terminal_pc=$((terminal))" +permissive-off "$elf" > "$log" 2>&1; then
    printf 'Native %s simulation failed; see %s\n' "$profile" "$log" >&2
    exit 1
  fi
  python3 -B "$src/check_report.py" "$profile" "$log" "$name.symbols" > "$name.verdict.json"
  printf '%s: Pass (%s)\n' "$name" "$log"
done
