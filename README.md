# sdcprobe

**A CPU diagnostic that asks two narrow questions: does your processor read the
legacy high-byte register `CH` correctly, and does a register that no
instruction writes keep its value?**

On a healthy x86-64 CPU both questions are trivial — the answer is always yes,
by definition of the instruction set. On some faulty CPUs it is not. `sdcprobe`
runs one core at a time and has one probe for each question. The first verifies
every `CH` read against the same data taken from memory and tells you exactly
which byte came back wrong. The second runs a function that keeps its frame
base in `rbx` and catches the moment `rbx` stops being that base.

[![build](https://github.com/atomontage04/sdcprobe/actions/workflows/ci.yml/badge.svg)](https://github.com/atomontage04/sdcprobe/actions/workflows/ci.yml)

---

## The one-paragraph version

`movd %xmm0, %ecx` writes 32 bits into `ECX`. `movzbl %ch, %esi` then reads bits
8..15 of that same register. The result is uniquely determined — there is
nothing to vary. On the machine this tool came from, roughly **one such read in
10⁷ returned `00` instead of the real byte**, while the other three bytes of the
same `ECX` were correct at that same instant. That cannot be a software bug. It
is the silicon getting a defined operation wrong, rarely, under load, at high
boost clocks.

If your machine does that, you will not notice directly. You will notice
corrupted archives, compilers that fail once in a hundred builds, checksums that
do not match on retry, and games that crash in ways nobody else can reproduce.

## The second probe, in one paragraph

A function whose frame is both over-aligned and dynamically sized keeps the
frame base in `rbx`: `sub $0xa0,%rsp` then `mov %rsp,%rbx`, and every local is
addressed through `rbx` from there on. On the same machine, after the `CH` fault
had been tuned away by lowering the core multiplier, such a function would —
once in a few billion calls — find `rbx` equal to `rsp + 0xa0`: the value `rsp` had
*before* the `sub`. No instruction writes that value into `rbx`, and it exists
nowhere in memory or in any saved context; it exists only inside the processor.
The function then reads its locals out of its caller's frame, and the program
dies of whatever those bytes happen to be. This is not silent the way the `CH`
fault is — it ends in a crash — but the crash is somewhere else every time, and
nothing about it points at the CPU.

## Silent Data Corruption

This is the class of fault the name refers to: a CPU computes a well-defined
operation and returns a value that is simply wrong — no crash, no exception, no
line in any error log, nothing an operating system can act on. The processor
does not know it happened, so nothing downstream does either, until a checksum
fails to match on retry or a build breaks in a way that does not survive a
rerun.

Silent Data Corruption (SDC) was brought to wide attention and studied at fleet
scale by Google, in "Cores that Don't Count" (HotOS 2021), and independently
by Meta, in "Silent Data Corruptions at Scale" (2021) — both describing
production machines that passed every qualification test and still, rarely,
computed the wrong answer.
`sdcprobe` — the name is that acronym plus "probe" — does not diagnose SDC in
general. It narrows the same class of fault down to cases small enough to
reason about completely: a single byte read through the legacy `CH` register,
and a single register that changes between two instructions that do not touch
it.

## Should you run this?

Run it if you have a machine that misbehaves in ways that look like memory
corruption but pass every memory test, especially if:

- builds, compressions or archive extractions fail intermittently and
  unreproducibly;
- all-core stress tests (Prime95, y-cruncher, OCCT) pass cleanly — a fault that
  only appears on a *single* core at maximum boost is invisible to them, because
  all-core load drops the clock;
- the CPU is one where this class of instability has been reported. Intel's
  13th and 14th generation desktop parts — widely discussed online as "Raptor
  Lake instability" — are the obvious examples to check. The original case
  here was an i9-14900K, though nothing in the test itself is specific to that
  chip.

Do **not** run it expecting a general verdict on your CPU. It tests two
specific things. See [What this tool does not do](#what-this-tool-does-not-do).

## Quick start

### Prebuilt binaries

Download from [Releases](https://github.com/atomontage04/sdcprobe/releases), verify against `SHA256SUMS.txt`, then:

```sh
# Prove the tool itself works. Takes a second, needs no faulty hardware.
sdcprobe --self-test

# Then the real thing. Answer the three questions it asks.
sdcprobe
```

Run it on the machine itself, not in a virtual machine. That includes WSL2: a
guest CPU is not a physical core, the hypervisor moves it between cores as it
sees fit, and "core 3" in the report would mean nothing. On Windows, use the
Windows binary.

The Windows build is static and needs only `KERNEL32.dll` and `msvcrt.dll`.

### From source

You need CMake 3.25+ and GCC or Clang. See
[Building from source](#building-from-source) for Windows specifics.

```sh
git clone https://github.com/atomontage04/sdcprobe
cd sdcprobe
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/sdcprobe --self-test
```

## Usage

With no arguments it asks three questions and gets out of the way:

```
$ sdcprobe
sdcprobe 1.1.0 (4c94538)  |  x86_64-w64-mingw32 gcc 13-win32  |  windows
CPU: Intel(R) Core(TM) i9-14900K
Logical cores available to this process: 32   [0-31]

Probe - 'ch' (CH misread), 'rbx' (frame base) or 'all' [ch]: rbx
Cores to test - 'all', or a list like '0,1,2' or '0-7,16' [all]: 0-7
Minutes per core [10]: 20
```

The probes are separate runs on purpose. They look for different faults, found
at different settings of the same machine, and each has its own report. `all`
runs both on each core, one after the other, and takes twice as long.

Core selection accepts `all`, a list (`0,1,2` — commas and spaces are
interchangeable), ranges (`0-7`), or a mix (`0-3,16,20-23`). Empty input takes
the default.

Everything can be given on the command line instead; whatever you specify is
not asked about:

| flag | meaning | default |
|---|---|---|
| `--probe NAME` | `ch`, `rbx`, or `all` for both in turn | `ch` |
| `--cores SPEC` | which logical cores to test | `all` |
| `--minutes N` | duration **per core and probe**, 1..1440 | `10` |
| `--seed N` | `ch` workload seed; changes the data, not the test | `42` |
| `--layers N` | `ch` workload size, 1..64 | measured at startup |
| `--log PATH` | report file | `sdcprobe-YYYYMMDD-HHMMSS.log` |
| `--self-test` | verify both detectors, then exit | — |
| `--version`, `--help` | | — |

```sh
sdcprobe --cores all --minutes 20
sdcprobe --probe rbx --cores 0-7,16 --minutes 30 --log run1.log
sdcprobe --probe all --cores 8 --minutes 60
```

Exit codes:

| code | meaning |
|---:|---|
| 0 | clean, sweep completed |
| 1 | fault detected, by either probe |
| 2 | bad arguments, or no core could be measured |
| 3 | self-test failed |
| 4 | interrupted before finishing, nothing found |

### Rules for a run that means something

**Leave the machine idle.** This is the single most important thing. Unrelated
load drops the core clock and hides the fault outright: during the original
investigation the same binary produced **0 detections in 19101 rounds** while
other work was running, and **231 in 12701 rounds** on its own.

**Give it time, and repeat it.** The fault comes in bursts. Three runs of
20 minutes per core is the minimum worth trusting. One clean 10-minute run
proves very little.

**Test every logical core too**, not only physical cores.
On the machine this tool came from, two SMT siblings of the same physical core,
at the same multiplier, differed by two orders of magnitude.
11 detections in ten minutes on one, over a thousand on the other.
Sharing execution units does not mean sharing the fault rate.

**Expect heat.** One core is pinned at 100% for the whole duration, which is
exactly the condition that provokes the fault. Ctrl+C stops after the current
round and still prints a report.

**Not in a virtual machine.** Pinning to a guest CPU does not select a physical
core. Under WSL2 the rbx fault showed up just as often with the process pinned
to one guest CPU as without pinning — which is how it was found, and why it
could not be attributed to a core from there.

**Time it out.** `--cores all --minutes 20` on a 32-thread CPU is over ten
hours. The tool prints the total before starting.

In the shipped configuration (250 ns pacing) the original faulty core produced
11 detections in 2.44·10⁹ checks — about 10 per 10 minutes. A core with less
margin loss will take proportionally longer, which is why three runs of
20 minutes is the floor rather than the target.

## Reading the output

Progress goes to the screen and to the report file at the same time, every
30 seconds. The file exists because a full sweep runs for hours and nobody
watches it — by the time something is found the console has scrolled away.

```
--- cpu 3  (1 of 8) ---
[13:36:46]   cpu 3   [    30s / 1200s ]  rounds 499      detections 0
...
  DETECT cpu 3 round 1712: lane 1 of sample 16610 — memory 26, register 00
...
=========================== SUMMARY ===========================
  cpu    rounds       checks         detections   L0    L1    L2    L3   multi
  3      2411         241100000      3            0     3     0     0     0
  4      2417         241700000      0            0     0     0     0     0
---------------------------------------------------------------
CHECKS PERFORMED:  482800000   (one check = one CH read verified against memory)
detections:        3
lanes:             L0(cl) 0   L1(ch) 3   L2(shr16) 0   L3(shr24) 0   multi-byte 0
value drift:       0   (samples differing from round 0)

RESULT: CH MISREAD DETECTED — 3 detections in 482800000 checks across 2 core(s)
```

A `DETECT` line names the exact failure: which byte of which 4-byte value, what
memory held, and what the register returned.

The **lane counters** are the interesting part. Lane 1 is the byte read through
`CH`; lanes 0, 2 and 3 are read from the same `ECX` by ordinary means. In the
original case every single detection landed on lane 1 and none on the others,
which is what pinned the fault to the high-byte read rather than to `ECX` as a
whole. If your lanes come out spread evenly, you are looking at something else.

**value drift** counts samples whose computed value changed between rounds. In
the original case it stayed at zero through hundreds of detections: the data was
never corrupted, only the register read was. A non-zero value here means
something broader is wrong.

`RESULT: CLEAN` means the fault did not reproduce in the time given. It is not a
clean bill of health.

### The rbx probe

```
--- cpu 0  (1 of 1) ---
[05:37:36]   cpu 0   [    30s / 900s ]  passes 5716     detections 0
...
  DETECT cpu 0 pass 44297: access violation at emit_i16+0x229, rbx-rsp=0xa0 rbp-rsp=0xb0  [frame base moved by 0xa0]
...
======================== SUMMARY: rbx ========================
  cpu    passes       calls          detections   frame-base  other  hash
  0      172841       45309231104    3            3           0      0
---------------------------------------------------------------
CALLS PERFORMED:   45309231104   (one call = one frame set up, used and torn down through rbx)
detections:        3
kinds:             frame base moved by 0xa0: 3   other fault: 0   wrong hash: 0

RESULT: RBX FAULT DETECTED - 3 detections in 45309231104 calls across 1 core(s)
```

(That run was made under WSL2, where "cpu 0" is a guest CPU and says nothing
about which core failed. It is shown for the shape of the report.)

A `DETECT` line is a hardware exception raised by the probe's own code, with
the registers at that moment. `emit_i16+0x229` is the instruction; the two
differences are what matter. Inside `emit_i16` the frame base and the stack
pointer are the same value by construction, so `rbx-rsp` must be 0. **`0xa0` is
the fault's signature**: it is the size the prologue subtracted, and `rbx` is
holding the stack pointer from before the subtraction. In the original case all
51 failures observed before this probe existed had exactly these two
differences, `0xa0` and `0xb0`, and so has every one caught by it since.

The **kinds** line splits detections three ways. *Frame base* is the signature
above. *Other fault* is an exception anywhere else in the probe's code, or with
other register values — the same fault can surface like that if the wrong frame
happens to hold different bytes, but so can a different problem. *Wrong hash* is
a pass that ran to completion and produced a wrong result; in the original case
that never happened, not once. If your detections are mostly not in the first
column, you are looking at something else.

After each detection the probe rebuilds all of its state and carries on, so one
run can count many. If the tool itself dies during an rbx run — no summary, the
report file simply stops — treat that as a detection on the core named in the
last line of the file: the same fault can leave the stack pointer unusable, and
then the operating system cannot even deliver the exception.

## How it works

Each round computes 100000 values of a synthetic workload. Every value goes down
two paths at once:

- into an **FNV-1a hash whose bytes are taken from the register**, via
  `movd`/`CL`/`CH`/shifts — the path that can fail;
- into an **array, by `movss` straight from XMM**, bypassing `ECX` entirely —
  the path that is always intact.

At the end of the round the same FNV-1a is computed over that array in one
straight pass through memory. Two hashes over identical bytes must agree.
A mismatch is a detection.

That comparison is the whole trick: it verifies the register path against the
memory path inside a single pass over unchanged data, so there is nothing else
left to blame.

Then the hash is **inverted**. The FNV prime is odd and therefore invertible
modulo 2⁶⁴, so from the corrupted final value plus the known-good byte stream
the tool reconstructs what the accumulator must have been at every step. The
difference at the failing step is the error itself:

| difference | meaning |
|---|---|
| below 256 | one byte was misread; the difference is `correct XOR observed` |
| popcount 1 | a single bit of the accumulator flipped |
| anything else | not one isolated event — two or more bytes in that round |

A false positive here has probability on the order of 400000 · 2⁻⁵⁶.

The hot loop is written as inline assembly rather than left to the compiler. The
exact instruction sequence *is* the measurement; a compiler is free to take the
bytes from memory instead, which would leave nothing to test.

### The rbx probe

The probe's code is a small hash loop: 64 arrays of 4096 16-bit values, each
value passed through `emit_i16`, which copies it into a two-byte local buffer
and hands the buffer to the hash. One pass is 262144 calls and takes about 5 ms.
Nothing in it is interesting except the shape of `emit_i16` as compiled at `-O0`
under AddressSanitizer:

```
push %rbp ; mov %rsp,%rbp ; push %rbx
and  $-32,%rsp
sub  $0xa0,%rsp
mov  %rsp,%rbx              frame base; every local is addressed through rbx
mov  %rdi,0x80(%rbx)
call __asan_stack_malloc_0  inside: push %rbx ... rbx used as scratch ... pop %rbx
...
mov  0x80(%rbx),%rcx
call <hash step>            does not touch rbx at all
mov  0x38(%rbx),%rdx        <- in every observed failure rbx is rsp+0xa0 by here
...
lea  -0x8(%rbp),%rsp ; pop %rbx ; pop %rbp ; ret
```

**The machine code is carried as bytes, not compiled.** This is the unusual
part, and it is deliberate. A hand-written assembly imitation of the same
prologue, the same call with `push`/`pop %rbx`, and the same frame accesses was
run next to the original for 10¹⁰ calls and failed zero times while the
original kept failing. Whatever the trigger is, it lives in the exact
instruction stream, and that stream comes from one compiler version and from
the ASan runtime, which MinGW does not even have. So
[`src/rbx_payload.S`](src/rbx_payload.S) holds the sixteen functions one pass
executes — the loop, `emit_i16`, and the piece of the ASan runtime called on
every iteration — copied instruction by instruction out of the binary that
failed, each at its original address modulo 4096.
[`tools/gen_rbx_payload.py`](tools/gen_rbx_payload.py) produced it and documents
the three things that are not verbatim: displacements between functions, the
read of the thread pointer (`%fs:0` becomes `%gs:0x28`, so the same bytes run
on Windows), and `memset`, whose zero-length path is reproduced rather than
copied. Linux and Windows
binaries contain the same payload bytes.

Whether a transplant still provokes the fault can only be settled by running
it, so that was done: on the machine that has the fault, the original
reproducer and the transplanted code were run alternately, 15 to 30 seconds
each, so that both saw the same conditions.

| what ran | failures | run time |
|---|---:|---:|
| the original reproducer, under AddressSanitizer | 5 | 690 s |
| the payload with the original `%fs` read | 4 | 660 s |
| the payload as shipped, `%gs` read | 3 | 1020 s |
| `sdcprobe --probe rbx` itself, one run | 3 | 900 s |

All fifteen stopped at the same instruction with the same two register
differences. The counts are too small to rank the rates, and the fault's own
burstiness is larger than the gaps between them; what the table establishes is
that the shipped bytes do fail, outside AddressSanitizer, and are caught. All of
it was measured under WSL2, the only place the fault had been seen; nothing here
says how often it shows on a pinned physical core.

That code expects to live inside an ASan process, so the probe gives it the
three things it actually uses: shadow memory at the fixed address ASan computes,
a "fake stack" for the per-call frames, and the data laid out as the
`std::vector` objects it was compiled against. Everything else it could call —
every sanitizer report function — is replaced by a trap, so that arriving there
is itself caught and named.

**Detection is an exception, not a comparison.** A function reading its locals
from the wrong frame does not produce a subtly wrong hash; it dereferences
garbage within a few instructions. The probe catches the resulting hardware
exception (a signal handler on Linux, a vectored exception handler on Windows),
records the registers, rewinds to before the pass, rebuilds its memory from
scratch and continues. Each pass is also checked against a hash computed
independently, outside the payload.

**It needs AVX2**, because the copied path runs through two AVX instructions of
`memset`, and it needs two fixed address ranges to be free, at 96 TiB and at
12 TiB — which they are on Linux and on Windows 8.1 and later. Where either
condition fails the probe refuses to start and says why.

### Why one core at a time

The tool is single-threaded on purpose. The fault appears on a single core at
high boost, and any concurrent load suppresses it. Testing cores in parallel
would be 32× faster and would find nothing. So the selected cores are visited in
sequence, each pinned with `sched_setaffinity` on Linux or
`SetThreadGroupAffinity` on Windows, and the pin is verified rather than assumed.

Only cores actually available to the process are listed. Under `taskset`, in a
container with a restricted cpuset, or with an affinity mask already applied, the
list will be shorter — which is correct, since the rest could not be pinned
anyway.

### Why there is a synthetic workload

The fault does not reproduce on the bare instruction pair. Measured on the
machine where it was present:

| context | 1 error per … CH reads |
|---|---:|
| bare `movd`/`movzbl %ch`, no surrounding work | not seen in 2.0·10⁹ |
| workload present, loop diluted with branches | 2.5·10⁸ |
| exact compiler-emitted sequence, tight loop | 1.7·10⁷ |

So the surrounding work matters, and it appears to matter through core power
draw at high boost rather than through the instruction mix. The workload exists
to hold the gap between consecutive `CH` reads at roughly **250 ns**, the
interval at which the fault was originally observed.

Different CPUs run it at different speeds, so the layer count is **measured at
startup** and reported:

```
calibration: 5 layers -> 247 ns per check (target 250)
```

Override with `--layers N` if you want a specific value. If the achieved
interval drifts more than 2.5× from the target, the tool says so in the summary,
because a clean result at the wrong pacing is worth less.

### The self-test

```sh
sdcprobe --self-test
```

This is not a formality. Without it, a clean sweep is indistinguishable from a
tool that silently detects nothing — a bad build, an inlined workload, a mistake
in the reverse analysis. The self-test injects misreads of known shape into the
byte stream and requires the analysis to name each one exactly: lane, sample
index, byte before, byte after. It also injects two bytes at once and requires
that this *not* be reported as a single-byte misread.

For the rbx probe it does the equivalent: it runs one clean pass and checks the
hash, then makes `emit_i16` get back an `rbx` that is `0xa0` too high on its
1000th call — the same change the fault makes — and requires that the
exception is caught and that the next pass is clean again. That exercises the
whole path a real detection takes, including the parts that differ between
Linux and Windows.

It needs no faulty hardware. Run it first, on any machine, to confirm your
binary is sound.

## What this tool does not do

- **It does not test your CPU in general.** Two specific faults, each in one
  setting. Use memtest, Prime95 and friends for everything else.
- **It does not diagnose a cause.** A detection tells you a defined operation
  returned the wrong value. Voltage, frequency, temperature, microcode and
  cooling are all candidates and none is established here.
- **A clean result is not a guarantee.** It means the fault did not reproduce in
  the time given, on the cores tested, at the pacing achieved.
- **It is not a fix.**
  If it detects something, what to do about it is out of scope.
- **The rbx probe is one build of one function.** It reproduces a fault seen on
  one machine with one instruction stream. Nothing is known about how general
  the underlying defect is, and a CPU could have it and never trip on this
  particular stream.
- **x86-64 only, GCC or Clang only.** MSVC has no x64 inline assembler; the
  build fails at configure time with an explanation. Non-x86 architectures have
  no `CH` register, so the question does not exist there.

## FAQ

**Is this the same thing as the Intel 13th/14th Gen "Raptor Lake instability"
everyone was reporting in 2023–2024?**
Related, not proven identical. That issue covers a range of symptoms, and Intel
has publicly attributed at least one confirmed mechanism to it — "Vmin Shift
Instability", caused by elevated operating voltage, addressed with microcode
and BIOS updates. `sdcprobe` tests one specific, narrower thing: whether a
single register read comes back correct. A machine can have that problem and
pass this test, or the other way around: they are not the same question. See
[Silent Data Corruption](#silent-data-corruption).

**Does `RESULT: CLEAN` mean my CPU is fine?**
No. It means the fault did not reproduce in the time given, on the cores
tested, at the pacing achieved — see
[What this tool does not do](#what-this-tool-does-not-do). The fault is
bursty; a short run proves very little.

**Does this run on AMD? On ARM?**
Any x86-64 CPU, Intel or AMD — the `CH` register exists on all of them. The
fault itself has so far only been reported on Intel 13th/14th generation
desktop parts; whether it exists elsewhere is unknown, not ruled out. ARM,
RISC-V and other non-x86 architectures have no legacy high-byte registers, so
the question this tool asks does not exist there.

## Background

This started as a determinism bug hunt in an unrelated project. A pure function
of constant inputs returned two different answers inside one process, about once
every 300 repetitions — impossible by construction, so either the code had
hidden state or the machine was wrong.

It had no hidden state. MemorySanitizer, Valgrind Memcheck with origin
tracking, `-fstack-protector-all` and a formal bounds audit all came back clean.
The corrupted value always turned out to be a single byte, always the same lane,
always read through `CH`, while the identical value written to memory from the
XMM register was intact every time. A build from a different compiler, which
emitted a byte loop from memory instead of extracting from `ECX`, never failed
at all across 29087 runs.

`sdcprobe` is that finding turned into a tool: same instruction sequence, same
pacing, self-verifying, portable, with the project-specific parts replaced by a
synthetic workload.

The second probe has the same origin, later. Lowering the core multiplier made
the `CH` fault stop reproducing, and for a while that looked like the end of it.
Then a test suite in the same unrelated project began failing under
AddressSanitizer now and then, each time with a different report: a
heap-buffer-overflow with a truncated stack, a stack-buffer-underflow, a "member
call on misaligned address", a jump to a garbage return address. The reports had
one thing in common once the register dumps were lined up: `rbx - rsp = 0xa0` in
a function where the two must be equal. Reduced to a single file,
[`rbx-repro/`](rbx-repro/) fails once every 15 to 110 seconds on that
machine when it runs alone, less with four busy threads beside it, and not at all with
sixteen — the same dependence on single-core boost as before. That directory is
kept as the provenance of the payload: it is the source the bytes were compiled
from.

A plausible location, not an established one: P-cores of this generation
execute `add`/`sub` with a small constant in the renamer, tracking a register
as "physical register plus offset". The value that turns up in `rbx` is exactly
the base without the offset.

## Building from source

### Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/sdcprobe --self-test
```

Clang works too: add `-DCMAKE_CXX_COMPILER=clang++`.

### Windows, natively (MSYS2)

MSVC cannot build this. Install [MSYS2](https://www.msys2.org/), then from the
**MINGW64** shell:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"
cmake --build build
./build/sdcprobe.exe --self-test
```

The static link flags are what make the resulting `.exe` runnable on a machine
with no MSYS2 installed — worth keeping if you plan to carry it to the machine
under test.

### Windows, cross-compiled from Linux

All presets in `CMakePresets.json` (`native`, `clang`, `win`) use the Ninja
generator, so install `mingw-w64` and Ninja first — `ninja-build` on
Debian/Ubuntu and Fedora, `ninja` on Arch. Then:

```sh
cmake --preset win
cmake --build --preset win
```

### A note on build flags

The rbx probe's payload, `src/rbx_payload.S`, is assembled by the same compiler
driver as everything else and needs nothing extra. It is a generated file that
is committed on purpose: regenerating it needs the exact compiler the fault was
found with (Ubuntu clang 21.1.8), and a payload from any other version is a
different instruction stream that may not provoke anything. To regenerate:

```sh
cd rbx-repro
clang++ -g -O0 -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer -o repro repro.cpp
clang++ -g -O0 -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer -c -o repro.o repro.cpp
python3 ../tools/gen_rbx_payload.py repro repro.o > ../src/rbx_payload.S
```

`CMAKE_INTERPROCEDURAL_OPTIMIZATION` is forced **off**, deliberately. The
workload lives in its own translation unit so that the call from the hot loop
stays an opaque call. If LTO inlines it, the compiler rebuilds the hot loop
including the instruction sequence under test, and the tool keeps running while
quietly measuring nothing. Do not turn it on.

## Contributing

If the tool itself misbehaves, the **Bug report** template asks for
`--self-test` output first, because that answers most questions immediately.

Questions about your own hardware are out of scope.
This repository cannot tell you what to do about your CPU.

## Related reading

- Google — "Cores that Don't Count" (HotOS 2021): the paper that put "Silent
  Data Corruption" on the map, describing the same class of fault found and
  studied across an entire fleet.
- Meta — "Silent Data Corruptions at Scale" (2021): an independent account of
  the same phenomenon from a different datacenter operator.
- Intel — public statements on "Vmin Shift Instability" affecting 13th and
  14th Gen desktop parts (2024): a different, already-diagnosed mechanism
  behind some of the same symptoms. See [FAQ](#faq).

## Layout

```
CMakeLists.txt            standalone project; LTO explicitly off
CMakePresets.json         native, clang and win presets (all use Ninja)
src/sdcprobe.cpp          prompts, core sweep, CH hot loop, reverse FNV analysis
src/load.hpp/.cpp         synthetic workload of the CH probe, separate translation unit
src/rbx.hpp/.cpp          rbx probe: the environment its payload runs in
src/rbx_payload.S         rbx probe: the payload, generated, see below
src/platform.hpp/.cpp     core enumeration, pinning, interrupts, fault capture
src/logger.hpp/.cpp       simultaneous screen and file output
src/version.hpp           version identity
tools/gen_rbx_payload.py  writes rbx_payload.S from a build of the reproducer
rbx-repro/                the reproducer the rbx payload was compiled from
```

## License

MIT — see [LICENSE](LICENSE).

## DISCLAIMER: STATUS

Unmaintained, published as-is. It was useful once, on one machine, and might
be useful to someone with the same symptoms. It is not a product: no roadmap,
no support, no commitment to fix anything, no interest in feature requests.
Fork it if you need it changed.

Written with heavy AI assistance and verified by measurement rather than by
review. Every number in this README came from a run: the detector was validated
on a known-good machine before being trusted on a suspect one, and the fault was
narrowed by elimination — hypervisor enabled and disabled, each core pinned in
isolation, three toolchains, MemorySanitizer, Valgrind with origin tracking,
and a formal bounds audit. That is the whole basis for trusting it. Judge
accordingly.

No warranty of any kind — see [LICENSE](LICENSE).