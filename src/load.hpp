#pragma once

// Synthetic compute load.
//
// The value it returns is never used for anything. Its only job is to make the
// gap between two consecutive reads of the CH register the same length it was
// in the case where the fault was originally found. That is not cosmetic: the
// observed fault rate collapsed as the surrounding context got thinner.
//
//   bare movd/movzbl %ch pair, no load        not seen in 2.0e9 reads
//   load present, loop diluted with branches  1 in 2.5e8
//   exact instruction sequence, tight loop    1 in 1.7e7
//
// Hence three requirements on this function, all mandatory:
//
//   1. One call takes roughly 250 ns. That was the interval between CH reads
//      in the original case.
//   2. Dense arithmetic: 64-bit multiplies mixed with floating point. The
//      trigger appears to be core power draw at high boost rather than the
//      instruction mix as such.
//   3. A SEPARATE translation unit and no LTO. If the compiler inlines this
//      function into the hot loop, it rebuilds that loop from scratch,
//      including the instruction sequence under test. The tool would still
//      look like it works and would quietly stop detecting anything. That is
//      exactly why only the declaration lives here.
//
// The signature (uint32_t, double, double) -> float mirrors the shape of the
// original call. It fixes not just the call itself but the argument setup in
// front of it (cvtsi2sd / mulsd / subsd), which is part of the measured loop.
//
// The function is pure and deterministic. That matters: the detector compares
// a hash built from register reads against a hash of the same values read from
// memory, and any nondeterminism in the load would make the comparison
// meaningless.

#include <cstdint>

// Octaves per layer. Layers restart from the base coordinate with their own
// salt so the coordinate never grows to a magnitude where the cast to int32
// would be undefined behaviour.
inline constexpr int k_load_octaves_per_layer = 4;

// Bounds on the layer count. The upper bound only exists so a bad calibration
// result cannot turn one call into something absurdly long.
inline constexpr int k_load_layers_min = 1;
inline constexpr int k_load_layers_max = 64;

// Default layer count, used when calibration is skipped. Measured at roughly
// 250 ns per call on an i9-14900K; other CPUs will differ, which is what
// calibration is for.
inline constexpr int k_load_layers_default = 5;

// Number of layers the next calls will use.
//
// This is mutable process-wide state, which is normally worth avoiding. It is
// deliberate here, and the alternative was worse: passing the count as an
// argument would add one instruction to the hot loop on every iteration, and
// the hot loop is the thing being measured. Reading it inside the load
// function costs nothing measurable, because that happens outside the
// instruction window under test.
//
// Contract: set once during calibration, before any measurement starts, and
// never touched again while a run is in progress.
void sdc_set_load_layers(int layers);
int sdc_load_layers();

float sdc_load_sample(uint32_t seed, double world_x, double world_y);
