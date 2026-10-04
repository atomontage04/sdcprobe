# rbx-repro: the standalone reproducer of the rbx fault

This directory is where the machine code of sdcprobe's rbx probe comes from.
`repro.cpp` is a single-file program that provokes the fault by itself, under
AddressSanitizer; the bytes in [`../src/rbx_payload.S`](../src/rbx_payload.S)
were cut out of a build of it. It is kept for two reasons: it is the source of
those bytes, and it is the control any change to the probe has to be compared
against.

You do not need it to use sdcprobe. Use it if you want to see the fault without
sdcprobe in the picture, or to regenerate the payload.

## Running it

```
./run.sh                      # 40 runs of 15 s
./run.sh 120 15               # half an hour
./run.sh 40 15 taskset -c 8   # pinned (see "Conditions")
```

The script builds `repro.cpp` with `clang++` (flags in the header of the file)
and runs it in a loop. One line per run: `clean`, or `FAULT` with the first line
of the report and the register differences. The full output of every faulty run
is kept in `logs/`. Exit code: 0 no fault, 1 at least one, 2 the build failed.

A fault looks like this, and the two differences were the same in all 51 cases
observed before the probe was written:

```
run   3     6.1 s  FAULT  exit 1  AddressSanitizer: SEGV  rbx-rsp=0xa0 rbp-rsp=0xb0  -> logs/run-…-3.log
```

## What the program does

It computes FNV-1a over vectors of 16-bit nodes and compares the hash of every
pass with the first one. Each node goes through `emit_i16` and a lambda. The
work that matters is not the hash but the shape of `emit_i16`: it has a local
array, so at `-O0` AddressSanitizer gives it a frame on its fake stack with a
dynamic `alloca` as the fallback, and the dynamic `alloca` makes the compiler
keep the frame base in `rbx`:

```
push %rbp ; mov %rsp,%rbp ; push %rbx
and  $-32,%rsp
sub  $0xa0,%rsp
mov  %rsp,%rbx              # frame base; every local is addressed through rbx
mov  %rdi,0x80(%rbx)        # the emit parameter
call __asan_stack_malloc_0  # inside: push/pop %rbx, and rbx as a scratch register between them
…                           # fake-frame header, shadow poisoning
mov  0x80(%rbx),%rcx        # the parameter is read back
call <lambda>               # does not touch rbx
…                           # the epilogue reads 0x48(%rbx), 0x38(%rbx)
lea  -0x8(%rbp),%rsp ; pop %rbx ; pop %rbp ; ret
```

`rsp` does not change after the prologue, so `rbx == rsp` must hold throughout
the body. About 50 million calls of `emit_i16` per second.

## What goes wrong

Somewhere between the prologue and the epilogue `rbx` becomes `rsp + 0xa0` —
the value `rsp` had before `sub $0xa0,%rsp`. The function then reads its slots
0xa0 bytes too high, out of its caller's frame. In this program the epilogue
takes garbage for the fake-frame pointer and dies with a SEGV on
`movb $0x0,(%rax)`; AddressSanitizer prints the registers, and that is where
`rbx − rsp = 0xa0` shows. As far as the sanitizer is concerned the process is
clean: this is not a finding of the sanitizer but its crash report about a
signal.

In other functions of the same shape — a different `N` in the `sub`, different
contents in the caller's slots — the same fault produces different reports:
`heap-buffer-overflow` with a truncated frame chain, `stack-buffer-underflow`,
"member call on misaligned address" from UBSan, a jump through a garbage return
address.

## Conditions

Seen on an Intel Core i9-14900K under WSL2. The rate is between one event per
15 s and one per 140 s of run time, alone on an otherwise idle machine, and it
drifts over minutes: a block of eight runs without a single event sits next to a
block where six of eight are red. Two rules follow: a short clean run proves
nothing, and conditions can only be compared by alternating runs. Alternated,
18 runs of 15 s per condition:

| condition | red |
|---|---|
| the program alone | 6 |
| 1 constantly busy thread beside it | 9 |
| 4 busy threads beside it (the program runs at the same speed) | 1 |
| 16 busy threads beside it | 0 |
| same binary, `ASAN_OPTIONS=detect_stack_use_after_return=0` | 0 |
| built with `-fsanitize-address-use-after-return=never` (no base in `rbx`) | 0 |

Built with `-O1`: 0 of 7. Under WSL2, pinning to a guest CPU does not select a
physical core (5 red of 10 with `taskset -c 2`): the hypervisor decides which
core a busy virtual processor runs on. Pinning by core only means something
without a hypervisor — which is what sdcprobe's rbx probe is for: run on Windows
itself, it caught the fault on the core where the `CH` fault had been found.

## Cause

Established: the value `rbx` receives is `rsp` from before the `sub`. It is not
in memory and not in any context saved by the kernel or the hypervisor; it
exists only in a physical register of the processor. The program is
single-threaded, there are no signals, and the same binary is green on a rerun.
Only the processor can supply such a value.

A hypothesis about where: the register renamer of the P-core. Golden Cove (and,
it appears, its refresh Raptor Cove) executes `add`/`sub` with a small constant
at the rename stage — a register is mapped as "physical register plus offset",
with the offset within ±1024
([Zero-cycle constant adds](https://www.complang.tuwien.ac.at/anton/additions/)).
`sub $0xa0,%rsp` falls in that range, `mov %rsp,%rbx` is eliminated at the same
stage, and the called function does `push`/`pop %rbx`. The observed value is the
base without the offset, which looks like the mapping of `rbx` being lost when
the renamer's state is restored. Presumably the same part of the core as the
`CH` read that sdcprobe's first probe checks. The hypothesis is not proven.

Reading the table: busy neighbours take the cores the host hands out first, and
the program ends up on a different core at the same frequency. Which core is
faulty cannot be seen from the guest.

## What is tied to what

- The fault is tied to the machine code that clang 21.1.8 emits at `-O0` with
  AddressSanitizer, and to the ASan runtime
  (`__asan_stack_malloc_0` → `PoisonShadow` → an indirect call of libc's
  `memset`). Another compiler version may lay `emit_i16` out differently;
  `run.sh` warns if the function has no `mov %rsp,%rbx`. Whether UBSan is
  needed was not tested: the flags are the ones the fault was caught with.
- A hand-written assembly imitation — the same prologue, a call of a function
  with `push`/`pop %rbx` and `rbx` as a scratch register, a nested call, writes
  to the "frame" and the "shadow", `cmp %rsp,%rbx` checks — did not catch it:
  0 events in 20 runs of 15 s (on the order of 10¹⁰ calls), in the same cycles
  where this program gave 3 red of 10 alone and 5 of 11 with one neighbour.
  That is why sdcprobe carries the original machine code as bytes instead of
  imitating it.
- The hash comparison (the `MISMATCH` line) has never fired. Every event so far
  ended in a signal.

## Regenerating sdcprobe's payload

```sh
clang++ -g -O0 -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer -o repro repro.cpp
clang++ -g -O0 -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer -c -o repro.o repro.cpp
python3 ../tools/gen_rbx_payload.py repro repro.o > ../src/rbx_payload.S
```

With the compiler named above this reproduces the committed file byte for byte.
With any other it produces a different payload, which has to be shown to fail
next to this program before it is worth anything.
