// sdcprobe — a detector for misreads of the legacy high-byte register CH on
// x86-64.
//
// What it looks for
// -----------------
// On a faulty CPU, reading a legacy high-byte register (CH) shortly after the
// full register (ECX) was written sometimes returns the wrong value — usually
// 00 — while the other three bytes of that same ECX are correct at that same
// moment. Architecturally that is impossible: movd writes ECX, movzbl %ch reads
// its bits 8..15, and the result is uniquely defined. So if a mismatch is
// observed, it is the machine getting it wrong, not the program.
//
// This was not found by looking for it. It came out of a symptom: a
// deterministic computation inside a single process returned two different
// answers about once every 300 repetitions. It narrowed down to one instruction
// in a hot loop emitted by gcc:
//
//     movd   %xmm0,%ecx     float bits -> ECX
//     movss  %xmm0,(...)    the same float -> an array
//     movzbl %cl,...        lane 0
//     movzbl %ch,...        lane 1   <-- the garbage came from here
//     shr    $0x18,%ecx     lane 3
//     shr    $0x10,...      lane 2
//
// How the check works
// -------------------
// Each round computes 100000 values of a synthetic load. Every value takes two
// paths at once: into an FNV-1a hash whose bytes are taken FROM THE REGISTER via
// ECX/CL/CH (the path that fails), and into an array by movss straight from XMM,
// BYPASSING ECX (the path that is always intact). At the end of the round the
// same FNV is computed over the array in one straight pass through memory. Two
// hashes over the same bytes must agree; a mismatch is a detection.
//
// The hash is then inverted — the prime is odd, hence invertible modulo 2^64 —
// and from the corrupted final value together with the known-good byte stream it
// reconstructs which byte was misread and what came back instead. That is where
// the lane number, the sample index and both byte values in the report come
// from.
//
// Why cores are tested ONE AT A TIME
// ----------------------------------
// The tool is single-threaded, and that is a condition of the measurement rather
// than a simplification. The fault shows up on a single core at high boost;
// unrelated load drops the clock and hides it completely. During the original
// investigation the same binary produced 0 detections in 19101 rounds alongside
// other work, and 231 in 12701 rounds on its own. So the selected cores are
// visited in sequence rather than all at once: a parallel sweep would be far
// faster and would find nothing.
//
// What this tool does NOT do
// --------------------------
// It does not test the CPU in general. It tests one instruction in one setting.
// A clean result means this particular fault did not reproduce in the time
// given, not that the machine is sound.

#include "load.hpp"
#include "logger.hpp"
#include "platform.hpp"
#include "version.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__x86_64__) && !defined(_M_X64)
#error "sdcprobe only makes sense on x86-64: it measures a read of the CH register"
#endif

#if defined(__x86_64__)
#include <cpuid.h>
#endif

namespace {

// Sample grid. 400 * 250 = 100000 load calls per round, roughly 25 ms — long
// enough for a round to carry statistical weight, short enough for progress to
// move.
constexpr uint32_t k_cols = 400u;
constexpr uint32_t k_rows = 250u;
constexpr double k_step = 3.5;
constexpr double k_origin_x = -700.0;
constexpr double k_origin_y = -437.5;

constexpr uint32_t k_default_seed = 42u;

constexpr uint64_t k_fnv_basis = 14695981039346656037ull;
constexpr uint64_t k_fnv_prime = 1099511628211ull;
// Inverse of the prime modulo 2^64, obtained by Newton iteration. The
// static_assert guards against a typo: with the wrong constant the reverse
// analysis would quietly turn into a random number generator.
constexpr uint64_t k_fnv_prime_inverse = 0xce965057aff6957bull;
static_assert(k_fnv_prime * k_fnv_prime_inverse == 1ull, "the modular inverse is wrong");

constexpr uint32_t k_default_minutes = 10u;
constexpr uint32_t k_max_minutes = 1440u;
constexpr int64_t k_progress_interval_s = 30;
// How many detections to spell out PER CORE. Beyond that only the counters: if
// everything is falling apart, a wall of text adds nothing to the first twenty.
constexpr uint32_t k_max_reported_per_cpu = 20u;

// Target time for one load call. That was the interval between CH reads in the
// case where the fault was found; drifting off it by a large factor reduces
// sensitivity.
constexpr double k_target_ns_per_sample = 250.0;
constexpr double k_ns_tolerance_factor = 2.5;

// Calibration probe size, in grid rows. 125 rows is 50000 samples, about 12 ms
// at the target rate — long enough to time reliably, short enough that startup
// stays instant.
constexpr uint32_t k_calibration_rows = 125u;

constexpr int k_exit_clean = 0;
constexpr int k_exit_detected = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_self_test_failed = 3;
constexpr int k_exit_interrupted = 4;

// Sanity bound on a core number in --cores. No real machine gets remotely
// close to this; it exists so that a typo like '0-9999999999' is rejected up
// front instead of filling `wanted` with billions of entries before the
// per-core lookup below would have rejected every one of them anyway.
constexpr long k_max_core_index = 4095l;

uint32_t popcount64(uint64_t value) {
    uint32_t count = 0u;
    while (value != 0ull) {
        value &= value - 1ull;
        ++count;
    }
    return count;
}

// The brand string lives in three extended CPUID leaves of 16 bytes each. Leaf
// 0x80000000 returns the highest available number — without that check the
// buffer would fill with junk from registers that were never updated.
void cpu_brand(char (&out)[49]) {
    std::memset(out, 0, sizeof(out));
#if defined(__x86_64__)
    unsigned int eax = 0u;
    unsigned int ebx = 0u;
    unsigned int ecx = 0u;
    unsigned int edx = 0u;
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000004u) {
        return;
    }
    for (unsigned int leaf = 0u; leaf < 3u; ++leaf) {
        if (__get_cpuid(0x80000002u + leaf, &eax, &ebx, &ecx, &edx) == 0) {
            std::memset(out, 0, sizeof(out));
            return;
        }
        std::memcpy(out + leaf * 16u + 0u, &eax, 4u);
        std::memcpy(out + leaf * 16u + 4u, &ebx, 4u);
        std::memcpy(out + leaf * 16u + 8u, &ecx, 4u);
        std::memcpy(out + leaf * 16u + 12u, &edx, 4u);
    }
#endif
}

const char* skip_leading_spaces(const char* text) {
    while (*text == ' ') {
        ++text;
    }
    return text;
}

const char* toolchain_name() {
#if defined(__clang__)
    return "clang";
#elif defined(__MINGW32__)
    return "x86_64-w64-mingw32 gcc " __VERSION__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown toolchain";
#endif
}

// ---------------------------------------------------------------------------
// Working memory
// ---------------------------------------------------------------------------

// Allocated once for the whole program and reused by every core: no allocator
// belongs in a measured loop, and the reverse-analysis buffers are 3.2 MB each.
struct Workspace {
    std::vector<float> samples;
    std::vector<float> baseline;
    std::vector<uint64_t> forward;
    std::vector<uint64_t> backward;

    explicit Workspace(size_t sample_count)
        : samples(sample_count, 0.0f), baseline(sample_count, 0.0f),
          forward(sample_count * sizeof(float) + 1u),
          backward(sample_count * sizeof(float) + 1u) {}
};

// ---------------------------------------------------------------------------
// The measured loop
// ---------------------------------------------------------------------------

// Computes `rows` grid rows, filling work.samples and accumulating the hash
// through the register path. Returns that accumulator.
//
// The row count is a parameter for one reason only: calibration needs a shorter
// pass. The inner loop over columns — the part actually under test — is
// identical either way.
uint64_t compute_rows(Workspace& work, uint32_t seed, uint32_t rows) {
    uint64_t acc = k_fnv_basis;
    float* const samples_data = work.samples.data();

    for (uint32_t j = 0u; j < rows; ++j) {
        const double world_y = k_origin_y + static_cast<double>(j) * k_step;
        for (uint32_t i = 0u; i < k_cols; ++i) {
            const double world_x = k_origin_x + static_cast<double>(i) * k_step;
            const float value = sdc_load_sample(seed, world_x, world_y);

            float& dst = samples_data[static_cast<size_t>(j) * k_cols + i];

            // The sequence is written out by hand. The order and choice of
            // instructions is the thing being measured; handing it to the code
            // generator is not an option, because it is free to take the bytes
            // from memory instead, leaving nothing to measure.
            uint64_t t0 = 0ull;
            uint64_t t1 = 0ull;
            uint64_t rcx_out = 0ull;
            __asm__ volatile("movd   %[f], %%ecx\n\t"
                             "movss  %[f], %[dst]\n\t"
                             "movzbl %%cl, %k[t0]\n\t"
                             "movzbl %%ch, %k[t1]\n\t"
                             "xor    %[acc], %[t0]\n\t"
                             "imul   %[prime], %[t0]\n\t"
                             "xor    %[t1], %[t0]\n\t"
                             "mov    %%ecx, %k[t1]\n\t"
                             "shr    $0x18, %%ecx\n\t"
                             "imul   %[prime], %[t0]\n\t"
                             "shr    $0x10, %k[t1]\n\t"
                             "movzbl %b[t1], %k[t1]\n\t"
                             "xor    %[t1], %[t0]\n\t"
                             "imul   %[prime], %[t0]\n\t"
                             "xor    %%rcx, %[t0]\n\t"
                             "imul   %[prime], %[t0]\n\t"
                             : [t0] "=&r"(t0), [t1] "=&r"(t1), "=c"(rcx_out), [dst] "=m"(dst)
                             : [f] "x"(value), [acc] "r"(acc), [prime] "r"(k_fnv_prime)
                             : "cc");
            acc = t0;
        }
    }
    return acc;
}

// The same FNV over the same bytes, but as one straight pass through memory.
// This path uses no high-byte registers and never failed.
uint64_t replay_hash(const float* data, size_t sample_count) {
    uint64_t hash = k_fnv_basis;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    const size_t total_bytes = sample_count * sizeof(float);
    for (size_t k = 0u; k < total_bytes; ++k) {
        hash ^= static_cast<uint64_t>(p[k]);
        hash *= k_fnv_prime;
    }
    return hash;
}

// Same thing, but with a chosen byte replaced. Used only by the self-test, to
// build the hash a misread would have produced.
uint64_t replay_hash_with_substitution(const float* data, size_t sample_count, size_t byte_index,
                                       unsigned char replacement) {
    uint64_t hash = k_fnv_basis;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    const size_t total_bytes = sample_count * sizeof(float);
    for (size_t k = 0u; k < total_bytes; ++k) {
        const unsigned char byte = (k == byte_index) ? replacement : p[k];
        hash ^= static_cast<uint64_t>(byte);
        hash *= k_fnv_prime;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Reverse FNV analysis
// ---------------------------------------------------------------------------

struct ReverseResult {
    bool single_byte = false;
    uint32_t lane = 0u;
    uint64_t sample_index = 0u;
    uint32_t stored = 0u;
    uint32_t observed = 0u;
    uint32_t min_popcount = 65u;
};

// The difference F_k ^ B_k at the failing step is the shape of the error:
//   below 256    -> the wrong BYTE was read, difference = b ^ b'
//   popcount 1   -> a bit of the accumulator flipped
//   otherwise    -> not a single event (two or more bytes in one round)
// At every other step the difference is pseudorandom, so a false positive has
// probability on the order of 400000 * 2^-56.
ReverseResult reverse_fnv(const std::vector<float>& correct_samples, uint64_t wrong_hash,
                          std::vector<uint64_t>& forward, std::vector<uint64_t>& backward) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(correct_samples.data());
    const size_t n = correct_samples.size() * sizeof(float);

    // F[k] — what the accumulator must have been before byte k.
    forward[0] = k_fnv_basis;
    for (size_t k = 0u; k < n; ++k) {
        forward[k + 1u] = (forward[k] ^ static_cast<uint64_t>(bytes[k])) * k_fnv_prime;
    }

    // B[k] — what the accumulator actually was, assuming the rest of the stream
    // was processed correctly.
    backward[n] = wrong_hash;
    for (size_t k = n; k > 0u; --k) {
        backward[k - 1u] =
            (backward[k] * k_fnv_prime_inverse) ^ static_cast<uint64_t>(bytes[k - 1u]);
    }

    ReverseResult result;
    for (size_t k = 0u; k <= n; ++k) {
        const uint64_t diff = forward[k] ^ backward[k];
        const uint32_t bits = popcount64(diff);
        if (bits < result.min_popcount) {
            result.min_popcount = bits;
        }
        if (diff != 0ull && diff < 256ull && !result.single_byte) {
            result.single_byte = true;
            result.lane = static_cast<uint32_t>(k % sizeof(float));
            result.sample_index = static_cast<uint64_t>(k / sizeof(float));
            result.stored = bytes[k];
            result.observed = bytes[k] ^ static_cast<unsigned char>(diff);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

// One timing probe at a given layer count, in nanoseconds per sample.
double probe_ns_per_sample(Workspace& work, uint32_t seed, int layers) {
    sdc_set_load_layers(layers);

    // One untimed pass first: it settles caches and lets the core reach its
    // working clock, so the timed pass is not measuring the ramp.
    volatile uint64_t sink = compute_rows(work, seed, k_calibration_rows);

    const auto start = std::chrono::steady_clock::now();
    sink = compute_rows(work, seed, k_calibration_rows);
    const auto finish = std::chrono::steady_clock::now();
    (void)sink;

    const double elapsed_ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    const double samples = static_cast<double>(k_calibration_rows) * static_cast<double>(k_cols);
    return elapsed_ns / samples;
}

// Picks a layer count that puts one load call near the target interval.
//
// Cost per call is close to linear in the layer count plus a fixed overhead, so
// two probes are enough to fit a line and solve for the target. A third probe
// then reports what was actually achieved rather than what was predicted.
int calibrate_layers(Workspace& work, uint32_t seed, Logger& log) {
    const int low_layers = 4;
    const int high_layers = 12;

    const double low_ns = probe_ns_per_sample(work, seed, low_layers);
    const double high_ns = probe_ns_per_sample(work, seed, high_layers);

    const double slope = (high_ns - low_ns) / static_cast<double>(high_layers - low_layers);
    int chosen = k_load_layers_default;
    if (slope > 0.0) {
        const double intercept = low_ns - slope * static_cast<double>(low_layers);
        const double ideal = (k_target_ns_per_sample - intercept) / slope;
        // Rounding to nearest, then clamping: the clamp is what keeps a
        // nonsensical fit from producing a call that runs for a millisecond.
        chosen = static_cast<int>(ideal + 0.5);
        if (chosen < k_load_layers_min) {
            chosen = k_load_layers_min;
        }
        if (chosen > k_load_layers_max) {
            chosen = k_load_layers_max;
        }
    }

    const double achieved = probe_ns_per_sample(work, seed, chosen);
    sdc_set_load_layers(chosen);

    log.line("calibration: %d layers -> %.0f ns per check (target %.0f)", chosen, achieved,
             k_target_ns_per_sample);
    return chosen;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

// Checks the detection and diagnosis machinery end to end on hardware that is
// working correctly.
//
// This matters more than it might look. Without it, a clean result is
// indistinguishable from a tool that silently detects nothing — a broken build,
// an inlined load, a mistake in the reverse analysis. The self-test injects
// misreads of known shape into the byte stream and demands that the analysis
// name each one exactly: lane, sample index, byte before and after.
bool self_test(Workspace& work, uint32_t seed, Logger& log) {
    log.line("self-test: verifying the detector against injected misreads");

    const size_t sample_count = work.samples.size();

    const uint64_t register_hash = compute_rows(work, seed, k_rows);
    const uint64_t memory_hash = replay_hash(work.samples.data(), sample_count);

    uint32_t passed = 0u;
    uint32_t failed = 0u;

    // Case 0: with no injection the two paths must agree. If they do not, this
    // machine just produced a real detection during the self-test, which is
    // worth saying out loud but is not a failure of the machinery.
    if (register_hash == memory_hash) {
        log.line("  PASS  clean round: register path and memory path agree");
        ++passed;
    } else {
        log.line("  NOTE  clean round: the two paths DISAGREE - that is a real detection,");
        log.line("        not a self-test failure. Run without --self-test to investigate.");
    }

    // Cases 1..N: a single injected byte, across all four lanes and across the
    // ends and the middle of the grid.
    struct Injection {
        uint64_t sample_index;
        uint32_t lane;
        unsigned char replacement;
    };
    const Injection injections[] = {
        {0ull, 0u, 0x00u},
        {0ull, 1u, 0x00u},
        {1ull, 2u, 0x00u},
        {1ull, 3u, 0x00u},
        {16610ull, 1u, 0x00u},
        {16610ull, 1u, 0x5du},
        {sample_count / 2u, 2u, 0xffu},
        {static_cast<uint64_t>(sample_count) - 1ull, 3u, 0x00u},
    };

    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(work.samples.data());

    for (const Injection& injection : injections) {
        const size_t byte_index =
            static_cast<size_t>(injection.sample_index) * sizeof(float) + injection.lane;
        const unsigned char original = bytes[byte_index];
        if (original == injection.replacement) {
            // Substituting a byte with itself changes no hash, so there would
            // be nothing to find. Skipping is correct, and saying so keeps the
            // pass count honest.
            log.line("  SKIP  sample %llu lane %u: stored byte already %02x",
                     static_cast<unsigned long long>(injection.sample_index), injection.lane,
                     injection.replacement);
            continue;
        }

        const uint64_t corrupted = replay_hash_with_substitution(
            work.samples.data(), sample_count, byte_index, injection.replacement);

        const ReverseResult r =
            reverse_fnv(work.samples, corrupted, work.forward, work.backward);

        const bool ok = r.single_byte && r.lane == injection.lane &&
                        r.sample_index == injection.sample_index && r.stored == original &&
                        r.observed == injection.replacement;
        if (ok) {
            log.line("  PASS  injected lane %u of sample %llu (%02x -> %02x), analysis agreed",
                     injection.lane, static_cast<unsigned long long>(injection.sample_index),
                     original, injection.replacement);
            ++passed;
        } else {
            log.line("  FAIL  injected lane %u of sample %llu (%02x -> %02x)", injection.lane,
                     static_cast<unsigned long long>(injection.sample_index), original,
                     injection.replacement);
            log.line("        analysis said: single_byte %d lane %u sample %llu %02x -> %02x",
                     r.single_byte ? 1 : 0, r.lane,
                     static_cast<unsigned long long>(r.sample_index), r.stored, r.observed);
            ++failed;
        }
    }

    // Last case: two injected bytes must NOT be reported as a single-byte
    // misread. Without this, an analysis that always answered "one byte" would
    // pass everything above.
    {
        const size_t first_byte = 1000u * sizeof(float) + 1u;
        const size_t second_byte = 50000u * sizeof(float) + 2u;
        uint64_t hash = k_fnv_basis;
        const size_t total_bytes = sample_count * sizeof(float);
        for (size_t k = 0u; k < total_bytes; ++k) {
            unsigned char byte = bytes[k];
            if (k == first_byte || k == second_byte) {
                byte = static_cast<unsigned char>(byte ^ 0x5au);
            }
            hash ^= static_cast<uint64_t>(byte);
            hash *= k_fnv_prime;
        }
        const ReverseResult r = reverse_fnv(work.samples, hash, work.forward, work.backward);
        if (!r.single_byte) {
            log.line("  PASS  two injected bytes reported as multi-byte, not single");
            ++passed;
        } else {
            log.line("  FAIL  two injected bytes reported as a single-byte misread"
                     " (lane %u sample %llu)",
                     r.lane, static_cast<unsigned long long>(r.sample_index));
            ++failed;
        }
    }

    log.blank();
    log.line("self-test: %u passed, %u failed", passed, failed);
    return failed == 0u;
}

// ---------------------------------------------------------------------------
// Per-core run
// ---------------------------------------------------------------------------

struct RunResult {
    uint32_t cpu_index = 0u;
    uint64_t rounds = 0ull;
    uint64_t checks = 0ull;
    uint64_t detections = 0ull;
    uint64_t lane_counts[4] = {0ull, 0ull, 0ull, 0ull};
    uint64_t unexplained = 0ull;
    uint64_t value_drift = 0ull;
    double elapsed_s = 0.0;
    double ns_per_check = 0.0;
    bool interrupted = false;
};

RunResult run_on_current_cpu(uint32_t cpu_index, uint32_t seed, uint32_t minutes, Logger& log,
                             Workspace& work) {
    RunResult result;
    result.cpu_index = cpu_index;

    const size_t sample_count = work.samples.size();
    uint32_t reported = 0u;

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::minutes(minutes);
    auto next_progress = start + std::chrono::seconds(k_progress_interval_s);

    while (std::chrono::steady_clock::now() < deadline) {
        // Checked once per round rather than per sample: a round is about 25 ms,
        // which is responsive enough, and a per-sample check would sit inside
        // the loop under test.
        if (sdc_interrupt_requested()) {
            result.interrupted = true;
            break;
        }

        const uint64_t register_hash = compute_rows(work, seed, k_rows);
        const uint64_t memory_hash = replay_hash(work.samples.data(), sample_count);

        // A second, independent check: whether the values themselves drift,
        // separately from the hash. During the original investigation this
        // stayed at zero through hundreds of detections — the data was never
        // corrupted, only the read was.
        if (result.rounds == 0ull) {
            work.baseline = work.samples;
        } else {
            for (size_t k = 0u; k < sample_count; ++k) {
                uint32_t a = 0u;
                uint32_t b = 0u;
                std::memcpy(&a, &work.baseline[k], sizeof(a));
                std::memcpy(&b, &work.samples[k], sizeof(b));
                if (a != b) {
                    ++result.value_drift;
                }
            }
        }

        if (register_hash != memory_hash) {
            ++result.detections;
            const ReverseResult r =
                reverse_fnv(work.samples, register_hash, work.forward, work.backward);
            if (r.single_byte) {
                ++result.lane_counts[r.lane];
            } else {
                ++result.unexplained;
            }
            if (reported < k_max_reported_per_cpu) {
                if (r.single_byte) {
                    log.line("  DETECT cpu %u round %llu: lane %u of sample %llu"
                             " - memory %02x, register %02x",
                             cpu_index, static_cast<unsigned long long>(result.rounds), r.lane,
                             static_cast<unsigned long long>(r.sample_index), r.stored, r.observed);
                } else {
                    log.line("  DETECT cpu %u round %llu: no single-byte explanation"
                             " (min popcount %u) - two or more bytes",
                             cpu_index, static_cast<unsigned long long>(result.rounds),
                             r.min_popcount);
                }
                ++reported;
                if (reported == k_max_reported_per_cpu) {
                    log.line("  (further detections on this cpu counted but not listed)");
                }
            }
        }

        ++result.rounds;

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_progress) {
            const int64_t elapsed_s =
                std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            log.progress("  cpu %-3u [ %5llds / %llds ]  rounds %-8llu detections %llu", cpu_index,
                         static_cast<long long>(elapsed_s),
                         static_cast<long long>(minutes) * 60ll,
                         static_cast<unsigned long long>(result.rounds),
                         static_cast<unsigned long long>(result.detections));
            while (next_progress <= now) {
                next_progress += std::chrono::seconds(k_progress_interval_s);
            }
        }
    }

    const auto finish = std::chrono::steady_clock::now();
    result.elapsed_s =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count()) /
        1000.0;
    result.checks = result.rounds * static_cast<uint64_t>(sample_count);
    result.ns_per_check = result.checks != 0ull
                              ? result.elapsed_s * 1.0e9 / static_cast<double>(result.checks)
                              : 0.0;
    return result;
}

// ---------------------------------------------------------------------------
// Core selection parsing
// ---------------------------------------------------------------------------

std::string trim(const std::string& text) {
    size_t first = 0u;
    while (first < text.size() &&
           (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' ||
            text[first] == '\n')) {
        ++first;
    }
    size_t last = text.size();
    while (last > first && (text[last - 1u] == ' ' || text[last - 1u] == '\t' ||
                            text[last - 1u] == '\r' || text[last - 1u] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

bool equals_ignore_case(const std::string& text, const char* literal) {
    size_t i = 0u;
    for (; i < text.size() && literal[i] != '\0'; ++i) {
        char a = text[i];
        char b = literal[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return i == text.size() && literal[i] == '\0';
}

// Parses "all", "0,1,2", "5", "0-7", "0-3, 16, 20-23".
// On false, `error` holds a message fit to show a human.
bool parse_cpu_selection(const std::string& input, const std::vector<CpuInfo>& available,
                         std::vector<CpuInfo>& out, std::string& error) {
    out.clear();

    const std::string text = trim(input);
    if (text.empty() || equals_ignore_case(text, "all")) {
        out = available;
        return true;
    }

    // Commas and semicolons are treated as spaces: people write "0, 1, 2" and
    // "0 1 2" alike, and demanding exact punctuation buys nothing.
    std::string normalized;
    normalized.reserve(text.size());
    for (const char c : text) {
        normalized.push_back((c == ',' || c == ';') ? ' ' : c);
    }

    std::vector<uint32_t> wanted;
    size_t pos = 0u;
    while (pos < normalized.size()) {
        while (pos < normalized.size() && normalized[pos] == ' ') {
            ++pos;
        }
        if (pos >= normalized.size()) {
            break;
        }
        const size_t token_start = pos;
        while (pos < normalized.size() && normalized[pos] != ' ') {
            ++pos;
        }
        const std::string token = normalized.substr(token_start, pos - token_start);

        const size_t dash = token.find('-');
        long low = 0;
        long high = 0;
        char* end = nullptr;

        if (dash != std::string::npos && dash != 0u) {
            const std::string low_text = token.substr(0u, dash);
            const std::string high_text = token.substr(dash + 1u);
            low = std::strtol(low_text.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || low_text.empty()) {
                error = "cannot parse '" + token + "'";
                return false;
            }
            high = std::strtol(high_text.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || high_text.empty()) {
                error = "cannot parse '" + token + "'";
                return false;
            }
            if (high < low) {
                error = "range '" + token + "' runs backwards";
                return false;
            }
        } else {
            low = std::strtol(token.c_str(), &end, 10);
            if (end == nullptr || *end != '\0') {
                error = "cannot parse '" + token + "'";
                return false;
            }
            high = low;
        }

        if (low < 0) {
            error = "negative core number in '" + token + "'";
            return false;
        }
        if (high > k_max_core_index) {
            error = "core number in '" + token + "' exceeds " + std::to_string(k_max_core_index);
            return false;
        }
        for (long value = low; value <= high; ++value) {
            wanted.push_back(static_cast<uint32_t>(value));
        }
    }

    if (wanted.empty()) {
        error = "no cores given";
        return false;
    }

    for (const uint32_t index : wanted) {
        const CpuInfo* found = nullptr;
        for (const CpuInfo& cpu : available) {
            if (cpu.index == index) {
                found = &cpu;
                break;
            }
        }
        if (found == nullptr) {
            error = "core " + std::to_string(index) + " is not available to this process";
            return false;
        }
        // A repeat is not an error, but there is no point running one core twice
        // in the same sweep.
        bool already = false;
        for (const CpuInfo& cpu : out) {
            if (cpu.index == index) {
                already = true;
                break;
            }
        }
        if (!already) {
            out.push_back(*found);
        }
    }
    return true;
}

// Compact core list: 0-7,16,20-23 instead of spelling every number out.
std::string format_cpu_list(const std::vector<CpuInfo>& cpus) {
    if (cpus.empty()) {
        return "(none)";
    }
    std::string text;
    size_t i = 0u;
    while (i < cpus.size()) {
        size_t j = i;
        while (j + 1u < cpus.size() && cpus[j + 1u].index == cpus[j].index + 1u) {
            ++j;
        }
        if (!text.empty()) {
            text += ",";
        }
        text += std::to_string(cpus[i].index);
        if (j > i) {
            text += "-" + std::to_string(cpus[j].index);
        }
        i = j + 1u;
    }
    return text;
}

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

// An empty return means input ran out, which happens with a redirected stdin.
// The caller has to tell that apart from a deliberate Enter, so the answer comes
// back through a parameter and the function returns success.
bool read_line(std::string& out) {
    char buffer[256];
    if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) {
        return false;
    }
    out = trim(std::string(buffer));
    return true;
}

void print_version() {
    std::printf("sdcprobe %s (%s), built with %s for %s\n", SDC_VERSION, SDC_GIT_HASH,
                toolchain_name(), sdc_platform_name());
}

void print_usage(std::FILE* out) {
    std::fprintf(
        out,
        "sdcprobe %s - detector for misreads of the legacy high-byte register CH (x86-64)\n"
        "\n"
        "  sdcprobe [--cores SPEC] [--minutes N] [--seed N] [--layers N] [--log PATH]\n"
        "  sdcprobe --self-test\n"
        "  sdcprobe --version | --help\n"
        "\n"
        "  --cores      which logical cores to test: 'all', or a list such as\n"
        "               '0,1,2' or '0-7,16'. Default: all\n"
        "  --minutes    duration PER CORE, 1..%u. Default: %u\n"
        "  --seed       workload seed. Default: %u. Changes the data, not the test\n"
        "  --layers     workload size, %d..%d. Default: measured at startup so that\n"
        "               one check takes about %.0f ns on this machine\n"
        "  --log        report file. Default: sdcprobe-YYYYMMDD-HHMMSS.log\n"
        "  --self-test  verify the detector against injected misreads and exit.\n"
        "               Needs no faulty hardware; proves the machinery works\n"
        "\n"
        "  With no arguments the tool asks interactively. Anything given on the\n"
        "  command line is not asked about. With a redirected stdin it never asks.\n"
        "\n"
        "  Cores are tested ONE AT A TIME, on purpose. The fault only shows on a\n"
        "  single core at high boost; concurrent load drops the clock and hides it\n"
        "  completely. Run this on an otherwise idle machine.\n"
        "\n"
        "  The fault is bursty. A single clean run proves little.\n"
        "\n"
        "  Ctrl+C stops after the current round and still prints a partial report.\n"
        "\n"
        "  Exit codes:  %d clean          %d misread detected\n"
        "               %d bad arguments  %d self-test failed   %d interrupted, nothing found\n",
        SDC_VERSION, k_max_minutes, k_default_minutes, k_default_seed, k_load_layers_min,
        k_load_layers_max, k_target_ns_per_sample, k_exit_clean, k_exit_detected, k_exit_usage,
        k_exit_self_test_failed, k_exit_interrupted);
}

} // namespace

int main(int argc, char** argv) {
    uint32_t seed = k_default_seed;
    uint32_t minutes = k_default_minutes;
    int layers = 0; // 0 means "calibrate at startup"
    std::string cores_spec;
    std::string log_path;
    bool have_minutes = false;
    bool have_cores = false;
    bool want_self_test = false;

    for (int i = 1; i < argc;) {
        const char* flag = argv[i];

        if (std::strcmp(flag, "--help") == 0 || std::strcmp(flag, "-h") == 0) {
            print_usage(stdout);
            return k_exit_clean;
        }
        if (std::strcmp(flag, "--version") == 0 || std::strcmp(flag, "-V") == 0) {
            print_version();
            return k_exit_clean;
        }
        if (std::strcmp(flag, "--self-test") == 0) {
            want_self_test = true;
            i += 1;
            continue;
        }

        // An unknown flag is recognised BEFORE a value is demanded, otherwise
        // 'sdcprobe --bogus' would complain about a missing value for a flag
        // that does not exist, sending the reader the wrong way.
        const bool known = std::strcmp(flag, "--minutes") == 0 ||
                           std::strcmp(flag, "--seed") == 0 ||
                           std::strcmp(flag, "--cores") == 0 ||
                           std::strcmp(flag, "--layers") == 0 || std::strcmp(flag, "--log") == 0;
        if (!known) {
            std::fprintf(stderr, "sdcprobe: unexpected argument '%s'\n", flag);
            return k_exit_usage;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "sdcprobe: %s expects a value\n", flag);
            return k_exit_usage;
        }
        const char* value = argv[i + 1];

        if (std::strcmp(flag, "--minutes") == 0) {
            char* end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (end == nullptr || *end != '\0' || parsed < 1 ||
                parsed > static_cast<long>(k_max_minutes)) {
                std::fprintf(stderr, "sdcprobe: --minutes expects 1..%u\n", k_max_minutes);
                return k_exit_usage;
            }
            minutes = static_cast<uint32_t>(parsed);
            have_minutes = true;
        } else if (std::strcmp(flag, "--layers") == 0) {
            char* end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (end == nullptr || *end != '\0' || parsed < k_load_layers_min ||
                parsed > k_load_layers_max) {
                std::fprintf(stderr, "sdcprobe: --layers expects %d..%d\n", k_load_layers_min,
                             k_load_layers_max);
                return k_exit_usage;
            }
            layers = static_cast<int>(parsed);
        } else if (std::strcmp(flag, "--seed") == 0) {
            seed = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
        } else if (std::strcmp(flag, "--cores") == 0) {
            cores_spec = value;
            have_cores = true;
        } else {
            log_path = value;
        }
        i += 2;
    }

    sdc_install_interrupt_handler();

    char brand[49];
    cpu_brand(brand);
    const char* brand_text = skip_leading_spaces(brand);

    const std::vector<CpuInfo> available = sdc_enumerate_cpus();
    if (available.empty()) {
        std::fprintf(stderr, "sdcprobe: could not determine any available core\n");
        return k_exit_usage;
    }

    Workspace work(static_cast<size_t>(k_cols) * static_cast<size_t>(k_rows));

    // Self-test runs on its own and exits. It needs no core selection, no
    // duration and no report file: it is a check of the tool, not of the
    // machine.
    if (want_self_test) {
        Logger console;
        print_version();
        console.line("CPU: %s", brand_text[0] != '\0' ? brand_text : "unknown");
        console.blank();
        sdc_set_load_layers(layers != 0 ? layers : k_load_layers_default);
        const bool ok = self_test(work, seed, console);
        console.blank();
        console.line("RESULT: SELF-TEST %s", ok ? "PASSED" : "FAILED");
        return ok ? k_exit_clean : k_exit_self_test_failed;
    }

    std::printf("sdcprobe %s (%s)  |  %s  |  %s\n", SDC_VERSION, SDC_GIT_HASH, toolchain_name(),
                sdc_platform_name());
    std::printf("CPU: %s\n", brand_text[0] != '\0' ? brand_text : "unknown");
    std::printf("Logical cores available to this process: %llu   [%s]\n",
                static_cast<unsigned long long>(available.size()),
                format_cpu_list(available).c_str());
    std::printf("\n");

    const bool interactive = sdc_stdin_is_tty();

    // Core selection.
    std::vector<CpuInfo> selected;
    for (;;) {
        std::string answer = cores_spec;
        if (!have_cores) {
            if (!interactive) {
                answer = "all";
                std::printf("Cores to test [all]: all   (stdin is not a terminal)\n");
            } else {
                std::printf("Cores to test - 'all', or a list like '0,1,2' or '0-7,16' [all]: ");
                std::fflush(stdout);
                if (!read_line(answer)) {
                    answer = "all";
                    std::printf("all   (end of input)\n");
                }
            }
        }
        std::string error;
        if (parse_cpu_selection(answer, available, selected, error)) {
            break;
        }
        std::printf("  %s\n", error.c_str());
        if (have_cores || !interactive) {
            return k_exit_usage;
        }
    }

    // Duration per core.
    while (!have_minutes) {
        std::string answer;
        if (!interactive) {
            std::printf("Minutes per core [%u]: %u   (stdin is not a terminal)\n",
                        k_default_minutes, k_default_minutes);
            break;
        }
        std::printf("Minutes per core [%u]: ", k_default_minutes);
        std::fflush(stdout);
        if (!read_line(answer)) {
            std::printf("%u   (end of input)\n", k_default_minutes);
            break;
        }
        if (answer.empty()) {
            break;
        }
        char* end = nullptr;
        const long parsed = std::strtol(answer.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || parsed < 1 ||
            parsed > static_cast<long>(k_max_minutes)) {
            std::printf("  expected an integer 1..%u\n", k_max_minutes);
            continue;
        }
        minutes = static_cast<uint32_t>(parsed);
        break;
    }

    if (log_path.empty()) {
        log_path = sdc_default_log_path();
    }

    Logger log;
    if (!log.open(log_path)) {
        std::printf("\nWARNING: could not open report file '%s'. Screen output only.\n",
                    log_path.c_str());
    }

    const uint64_t total_minutes = static_cast<uint64_t>(minutes) * selected.size();

    // The header was already on screen before the questions, but the report has
    // to carry it: the file goes to someone who never saw the console.
    log.file_line("sdcprobe %s (%s)  |  %s  |  %s", SDC_VERSION, SDC_GIT_HASH, toolchain_name(),
                  sdc_platform_name());
    log.file_line("CPU: %s", brand_text[0] != '\0' ? brand_text : "unknown");
    log.line("hot loop: movd %%xmm0,%%ecx / movss / movzbl %%cl / movzbl %%ch  (single thread)");
    log.line("report:   %s", log.has_file() ? log.path().c_str() : "(console only)");
    log.blank();
    log.line("cores:    %llu of %llu  [%s]", static_cast<unsigned long long>(selected.size()),
             static_cast<unsigned long long>(available.size()),
             format_cpu_list(selected).c_str());
    log.line("duration: %u min per core, %llu min total (%.1f h)", minutes,
             static_cast<unsigned long long>(total_minutes),
             static_cast<double>(total_minutes) / 60.0);
    log.line("seed:     %u", seed);

    // Calibration runs pinned to the first selected core, so it measures the
    // core the sweep is about to start on rather than wherever the scheduler
    // happened to leave the process.
    sdc_pin_to_cpu(selected.front());
    if (layers != 0) {
        sdc_set_load_layers(layers);
        log.line("layers:   %d (given on the command line, calibration skipped)",
                 sdc_load_layers());
    } else {
        calibrate_layers(work, seed, log);
    }

    log.blank();
    log.line("Cores are tested one at a time: concurrent load hides the fault.");
    log.line("Ctrl+C stops after the current round and still prints a report.");
    log.blank();

    std::vector<RunResult> results;
    results.reserve(selected.size());
    uint64_t skipped = 0ull;
    bool interrupted = false;

    for (size_t n = 0u; n < selected.size(); ++n) {
        if (sdc_interrupt_requested()) {
            interrupted = true;
            log.line("--- interrupted before cpu %u; %llu core(s) not tested ---",
                     selected[n].index,
                     static_cast<unsigned long long>(selected.size() - n));
            break;
        }

        const CpuInfo& cpu = selected[n];
        log.line("--- cpu %u  (%llu of %llu) ---", cpu.index,
                 static_cast<unsigned long long>(n + 1u),
                 static_cast<unsigned long long>(selected.size()));

        if (!sdc_pin_to_cpu(cpu)) {
            log.line("  SKIPPED: could not pin to core %u", cpu.index);
            ++skipped;
            continue;
        }
        const std::string pin_problem = sdc_verify_pin(cpu);
        if (!pin_problem.empty()) {
            // Not a refusal: the system may honour the request loosely. But
            // silently writing a core number into the report when the work ran
            // elsewhere is exactly the case where a report misleads.
            log.line("  WARNING: pinning not confirmed - %s", pin_problem.c_str());
        }

        const RunResult result = run_on_current_cpu(cpu.index, seed, minutes, log, work);
        results.push_back(result);
        if (result.interrupted) {
            interrupted = true;
        }

        log.line("  cpu %u done: rounds %llu, checks %llu, detections %llu,"
                 " lanes L0 %llu L1 %llu L2 %llu L3 %llu, multi-byte %llu,"
                 " value drift %llu, %.0f ns per check%s",
                 cpu.index, static_cast<unsigned long long>(result.rounds),
                 static_cast<unsigned long long>(result.checks),
                 static_cast<unsigned long long>(result.detections),
                 static_cast<unsigned long long>(result.lane_counts[0]),
                 static_cast<unsigned long long>(result.lane_counts[1]),
                 static_cast<unsigned long long>(result.lane_counts[2]),
                 static_cast<unsigned long long>(result.lane_counts[3]),
                 static_cast<unsigned long long>(result.unexplained),
                 static_cast<unsigned long long>(result.value_drift), result.ns_per_check,
                 result.interrupted ? "  (interrupted)" : "");
        log.blank();

        if (result.interrupted) {
            const size_t remaining = selected.size() - (n + 1u);
            if (remaining != 0u) {
                log.line("--- interrupted; %llu core(s) not tested ---",
                         static_cast<unsigned long long>(remaining));
            }
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Verdict
    // -----------------------------------------------------------------------

    uint64_t total_rounds = 0ull;
    uint64_t total_checks = 0ull;
    uint64_t total_detections = 0ull;
    uint64_t total_lanes[4] = {0ull, 0ull, 0ull, 0ull};
    uint64_t total_unexplained = 0ull;
    uint64_t total_drift = 0ull;
    double total_elapsed_s = 0.0;
    double worst_ns = 0.0;

    for (const RunResult& r : results) {
        total_rounds += r.rounds;
        total_checks += r.checks;
        total_detections += r.detections;
        for (uint32_t lane = 0u; lane < 4u; ++lane) {
            total_lanes[lane] += r.lane_counts[lane];
        }
        total_unexplained += r.unexplained;
        total_drift += r.value_drift;
        total_elapsed_s += r.elapsed_s;
        if (r.ns_per_check > worst_ns) {
            worst_ns = r.ns_per_check;
        }
    }

    log.line("=========================== SUMMARY ===========================");
    log.line("  cpu    rounds       checks         detections   L0    L1    L2    L3   multi");
    for (const RunResult& r : results) {
        log.line("  %-5u  %-11llu  %-13llu  %-11llu  %-5llu %-5llu %-5llu %-5llu %llu",
                 r.cpu_index, static_cast<unsigned long long>(r.rounds),
                 static_cast<unsigned long long>(r.checks),
                 static_cast<unsigned long long>(r.detections),
                 static_cast<unsigned long long>(r.lane_counts[0]),
                 static_cast<unsigned long long>(r.lane_counts[1]),
                 static_cast<unsigned long long>(r.lane_counts[2]),
                 static_cast<unsigned long long>(r.lane_counts[3]),
                 static_cast<unsigned long long>(r.unexplained));
    }
    log.line("---------------------------------------------------------------");
    log.line("cores tested:      %llu of %llu   (skipped %llu)",
             static_cast<unsigned long long>(results.size()),
             static_cast<unsigned long long>(selected.size()),
             static_cast<unsigned long long>(skipped));
    log.line("workload:          %d layers, %.0f ns per check", sdc_load_layers(), worst_ns);
    log.line("elapsed:           %.1f s (%.2f h)", total_elapsed_s, total_elapsed_s / 3600.0);
    log.line("rounds performed:  %llu", static_cast<unsigned long long>(total_rounds));
    log.line("CHECKS PERFORMED:  %llu   (one check = one CH read verified against memory)",
             static_cast<unsigned long long>(total_checks));
    log.line("detections:        %llu", static_cast<unsigned long long>(total_detections));
    log.line("lanes:             L0(cl) %llu   L1(ch) %llu   L2(shr16) %llu   L3(shr24) %llu"
             "   multi-byte %llu",
             static_cast<unsigned long long>(total_lanes[0]),
             static_cast<unsigned long long>(total_lanes[1]),
             static_cast<unsigned long long>(total_lanes[2]),
             static_cast<unsigned long long>(total_lanes[3]),
             static_cast<unsigned long long>(total_unexplained));
    log.line("value drift:       %llu   (samples differing from round 0)",
             static_cast<unsigned long long>(total_drift));

    // The interval between CH reads is part of the conditions under which the
    // fault is observable at all. Drifting off it does not make the result
    // wrong, but it makes a clean result weigh less, and that cannot go unsaid.
    if (worst_ns > 0.0 && (worst_ns > k_target_ns_per_sample * k_ns_tolerance_factor ||
                           worst_ns < k_target_ns_per_sample / k_ns_tolerance_factor)) {
        log.blank();
        log.line("WARNING: %.0f ns per check, expected around %.0f. The workload is not pacing",
                 worst_ns, k_target_ns_per_sample);
        log.line("this machine as intended, so sensitivity is reduced. Try --layers N.");
    }

    log.blank();
    if (results.empty()) {
        // Every core was skipped, so nothing was measured at all. Reporting
        // that as CLEAN would be the worst possible outcome: a green result
        // that means the opposite of what it says.
        log.line("RESULT: NOTHING TESTED - no core was measured");
        log.line("Pinning failed on every selected core, so no checks were performed. This is");
        log.line("not a clean result. Check whether the process is allowed to set affinity.");
        if (log.has_file()) {
            log.blank();
            log.line("Report written to %s", log.path().c_str());
        }
        log.close();
        return k_exit_usage;
    }
    if (total_detections != 0ull) {
        log.line("RESULT: CH MISREAD DETECTED - %llu detections in %llu checks across"
                 " %llu core(s)",
                 static_cast<unsigned long long>(total_detections),
                 static_cast<unsigned long long>(total_checks),
                 static_cast<unsigned long long>(results.size()));
        log.line("A byte read through the legacy high-byte register did not match the same byte");
        log.line("read from memory, within one pass over identical data. That cannot be a");
        log.line("software bug: the instruction sequence is deterministic.");
        log.line("Affected cores:");
        for (const RunResult& r : results) {
            if (r.detections != 0ull) {
                log.line("  cpu %u - %llu detections in %llu checks", r.cpu_index,
                         static_cast<unsigned long long>(r.detections),
                         static_cast<unsigned long long>(r.checks));
            }
        }
    } else if (interrupted) {
        log.line("RESULT: INTERRUPTED - no misread in %llu checks across %llu core(s) so far",
                 static_cast<unsigned long long>(total_checks),
                 static_cast<unsigned long long>(results.size()));
        log.line("The sweep did not finish, so this is not a clean result. It is a partial one.");
    } else {
        log.line("RESULT: CLEAN - no misread in %llu checks across %llu core(s)",
                 static_cast<unsigned long long>(total_checks),
                 static_cast<unsigned long long>(results.size()));
        log.line("This means the fault did not reproduce here, not that the machine is sound.");
    }

    if (log.has_file()) {
        log.blank();
        log.line("Report written to %s", log.path().c_str());
    }
    log.close();

    if (total_detections != 0ull) {
        return k_exit_detected;
    }
    if (interrupted) {
        return k_exit_interrupted;
    }
    return k_exit_clean;
}
