# sdcprobe

**A single-purpose CPU diagnostic: it checks whether your processor reads the
legacy high-byte register `CH` correctly.**

On a healthy x86-64 CPU that question is trivial — the answer is always yes,
by definition of the instruction set. On some faulty CPUs it is not. `sdcprobe`
runs one core at a time, verifies every read against the same data taken from
memory, and tells you exactly which byte came back wrong.

[![build](https://github.com/atomontage/sdcprobe/actions/workflows/ci.yml/badge.svg)](https://github.com/atomontage/sdcprobe/actions/workflows/ci.yml)

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

## Silent Data Corruption

This is the class of fault the name refers to: a CPU computes a well-defined
operation and returns a value that is simply wrong — no crash, no exception, no
line in any error log, nothing an operating system can act on. The processor
does not know it happened, so nothing downstream does either, until a checksum
fails to match on retry or a build breaks in a way that does not survive a
rerun.

Silent Data Corruption (SDC) was named and studied at fleet scale by Google, in
"Cores that Don't Count" (HotOS 2021), and independently by Meta, in "Silent
Data Corruptions at Scale" (2021) — both describing production machines that
passed every qualification test and still, rarely, computed the wrong answer.
`sdcprobe` — the name is that acronym plus "probe" — does not diagnose SDC in
general. It narrows the same class of fault down to one instruction small
enough to reason about completely: a single byte read through the legacy `CH`
register.

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

Do **not** run it expecting a general verdict on your CPU. It tests one
instruction. See [What this tool does not do](#what-this-tool-does-not-do).

## Quick start

### Prebuilt binaries

Download from [Releases](../../releases), verify against `SHA256SUMS.txt`, then:

```sh
# Prove the tool itself works. Takes a second, needs no faulty hardware.
sdcprobe --self-test

# Then the real thing. Answer the two questions it asks.
sdcprobe
```

The Windows build is static and needs only `KERNEL32.dll` and `msvcrt.dll`.

### From source

You need CMake 3.25+ and GCC or Clang. See
[Building from source](#building-from-source) for Windows specifics.

```sh
git clone https://github.com/atomontage/sdcprobe
cd sdcprobe
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/sdcprobe --self-test
```

## Usage

With no arguments it asks two questions and gets out of the way:

```
$ sdcprobe
sdcprobe 1.0.0 (abc1234)  |  gcc 15.2.0  |  linux
CPU: Intel(R) Core(TM) i9-14900K
Logical cores available to this process: 32   [0-31]

Cores to test — 'all', or a list like '0,1,2' or '0-7,16' [all]: 0-7
Minutes per core [10]: 20
```

Core selection accepts `all`, a list (`0,1,2` — commas and spaces are
interchangeable), ranges (`0-7`), or a mix (`0-3,16,20-23`). Empty input takes
the default.

Everything can be given on the command line instead; whatever you specify is
not asked about:

| flag | meaning | default |
|---|---|---|
| `--cores SPEC` | which logical cores to test | `all` |
| `--minutes N` | duration **per core**, 1..1440 | `10` |
| `--seed N` | workload seed; changes the data, not the test | `42` |
| `--layers N` | workload size, 1..64 | measured at startup |
| `--log PATH` | report file | `sdcprobe-YYYYMMDD-HHMMSS.log` |
| `--self-test` | verify the detector, then exit | — |
| `--version`, `--help` | | — |

```sh
sdcprobe --cores all --minutes 20
sdcprobe --cores 0-7,16 --minutes 30 --log run1.log
```

Exit codes:

| code | meaning |
|---:|---|
| 0 | clean, sweep completed |
| 1 | misread detected |
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

**Expect heat.** One core is pinned at 100% for the whole duration, which is
exactly the condition that provokes the fault. Ctrl+C stops after the current
round and still prints a report.

**Time it out.** `--cores all --minutes 20` on a 32-thread CPU is over ten
hours. The tool prints the total before starting.

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

It needs no faulty hardware. Run it first, on any machine, to confirm your
binary is sound.

## What this tool does not do

- **It does not test your CPU in general.** One instruction, one setting. Use
  memtest, Prime95 and friends for everything else.
- **It does not diagnose a cause.** A detection tells you a defined operation
  returned the wrong value. Voltage, frequency, temperature, microcode and
  cooling are all candidates and none is established here.
- **A clean result is not a guarantee.** It means the fault did not reproduce in
  the time given, on the cores tested, at the pacing achieved.
- **It is not a fix.** If it detects something, the next steps are the usual
  ones — stock or conservative voltage and power limits, current microcode and
  BIOS, and your vendor's warranty process.
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

`CMAKE_INTERPROCEDURAL_OPTIMIZATION` is forced **off**, deliberately. The
workload lives in its own translation unit so that the call from the hot loop
stays an opaque call. If LTO inlines it, the compiler rebuilds the hot loop
including the instruction sequence under test, and the tool keeps running while
quietly measuring nothing. Do not turn it on.

## Contributing

If the tool itself misbehaves, the **Bug report** template asks for
`--self-test` output first, because that answers most questions immediately.

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
CMakeLists.txt         standalone project; LTO explicitly off
CMakePresets.json      native, clang and win presets (all use Ninja)
src/sdcprobe.cpp       prompts, core sweep, hot loop, reverse FNV analysis
src/load.hpp/.cpp      synthetic workload, separate translation unit
src/platform.hpp/.cpp  core enumeration, pinning, interrupts: Linux and Windows
src/logger.hpp/.cpp    simultaneous screen and file output
src/version.hpp        version identity
```

## License

MIT — see [LICENSE](LICENSE).

## DISCLAIMER
This software was vibe-coded for personal use.
It helped a lot with debugging a CPU issue.

The author could not be held responsible for any issue that is caused by the software.