#!/usr/bin/env bash
# Builds repro.cpp with the flags the fault was observed with and runs it again and again.
# One line per run; the full output of every faulty run is kept in logs/.
#
# usage: run.sh [runs] [seconds-per-run] [prefix command...]
#   run.sh                       40 runs of 15 s
#   run.sh 120 15                half an hour
#   run.sh 40 15 taskset -c 8    pinned; a guest CPU of WSL2 is not a physical core, see README.md
# exit: 0 no fault, 1 at least one fault, 2 the build failed
set -u
here="$(cd "$(dirname "$0")" && pwd)"
runs="${1:-40}"
secs="${2:-15}"
[ $# -ge 1 ] && shift
[ $# -ge 1 ] && shift
cxx="${CXX:-clang++}"
bin="$here/repro"

if [ ! -x "$bin" ] || [ "$here/repro.cpp" -nt "$bin" ]; then
  "$cxx" -g -O0 -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -o "$bin" "$here/repro.cpp" || exit 2
fi
# The fault needs the frame base in rbx. Another compiler may lay emit_i16 out differently, and
# then a clean result says nothing.
if command -v objdump >/dev/null 2>&1; then
  objdump -d --no-show-raw-insn "$bin" | grep -A 12 'emit_i16.*>:$' | grep -q 'mov *%rsp,%rbx' \
    || echo "warning: emit_i16 of this build does not keep its frame base in rbx (no 'mov %rsp,%rbx')" >&2
fi

sym="$(command -v llvm-symbolizer || command -v addr2line || true)"
[ -n "$sym" ] && export ASAN_SYMBOLIZER_PATH="$sym" UBSAN_SYMBOLIZER_PATH="$sym"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"

# The first value of a register in the dump ASan prints with a SEGV.
reg() { grep -o "$1 = 0x[0-9a-f]*" "$2" | head -n 1 | awk '{print $3}'; }

mkdir -p "$here/logs"
faults=0
spent=0
for n in $(seq 1 "$runs"); do
  log="$here/logs/run-$(date +%Y%m%d-%H%M%S)-$n.log"
  t0="$(date +%s.%N)"
  "$@" "$bin" "$secs" >"$log" 2>&1
  rc=$?
  el="$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f", b - a }')"
  spent="$(awk -v a="$spent" -v b="$el" 'BEGIN { printf "%.1f", a + b }')"
  if [ "$rc" -eq 0 ]; then
    printf 'run %3d  %6s s  clean  (%s)\n' "$n" "$el" "$(grep -m 1 -o 'passes [0-9]*' "$log")"
    rm -f "$log"
    continue
  fi
  faults=$((faults + 1))
  what="$(grep -m 1 -o -E 'AddressSanitizer: [^ ]+|runtime error: .*|MISMATCH.*' "$log" | cut -c 1-80)"
  rbx="$(reg rbx "$log")"; rsp="$(reg rsp "$log")"; rbp="$(reg rbp "$log")"
  regs=""
  if [ -n "$rbx" ] && [ -n "$rsp" ] && [ -n "$rbp" ]; then
    regs="$(printf '  rbx-rsp=0x%x rbp-rsp=0x%x' $((rbx - rsp)) $((rbp - rsp)))"
  fi
  printf 'run %3d  %6s s  FAULT  exit %d  %s%s  -> %s\n' "$n" "$el" "$rc" "${what:-no report}" "$regs" "${log#"$here"/}"
done

if [ "$faults" -eq 0 ]; then
  printf 'RESULT: no fault in %d runs, %s s of run time\n' "$runs" "$spent"
  exit 0
fi
printf 'RESULT: %d faults in %d runs, %s s of run time, one fault per %s s\n' "$faults" "$runs" "$spent" \
  "$(awk -v t="$spent" -v f="$faults" 'BEGIN { printf "%.1f", t / f }')"
exit 1
