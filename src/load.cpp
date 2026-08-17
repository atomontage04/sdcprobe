// Synthetic load implementation. Rationale lives in load.hpp.
//
// This is an ordinary value-noise fBm: a lattice, four corners, quintic
// smoothing, a sum of octaves. It was not chosen for the quality of the
// result — nothing looks at the result — but because it produces the right
// mixture of work, 64-bit multiplies in the lattice hash plus floating point
// in the interpolation, and because the amount of it is easy to dial with the
// layer count.
//
// The lattice hash is splitmix64, picked as a self-contained mixer with two
// 64-bit multiplies, which dominate the cost of a call.

#include "load.hpp"

#include <cmath>

namespace {

int g_layers = k_load_layers_default;

// splitmix64, constants from the original publication.
uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Lattice node hash. Coordinates are packed into 64 bits and the layer salt is
// mixed in separately; without the salt every layer would land its nodes on
// the same points.
uint64_t lattice_hash(uint32_t seed, uint32_t salt, int32_t ix, int32_t iy) {
    const uint64_t packed = static_cast<uint64_t>(static_cast<uint32_t>(ix)) |
                            (static_cast<uint64_t>(static_cast<uint32_t>(iy)) << 32u);
    return splitmix64(packed ^ splitmix64(static_cast<uint64_t>(seed) ^
                                          (static_cast<uint64_t>(salt) << 32u)));
}

// Top 24 bits mapped into [0,1). Multiplying by a power of two is exact.
float unit_from_bits(uint64_t bits) {
    return static_cast<float>(static_cast<uint32_t>(bits >> 40u)) * 0x1p-24f;
}

float fade5(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float lerpf(float a, float b, float t) {
    return a + t * (b - a);
}

// Casting a float outside the range of int32 is undefined behaviour, not
// "some number". Layers restart from the base coordinate so no runaway should
// be possible, but that is not something to rely on: the clamp is explicit.
constexpr float k_lattice_limit = 1073741824.0f; // 2^30, well inside 2^31

float clamp_lattice(float value) {
    // Comparisons written as negations so NaN is clamped too: NaN fails every
    // comparison.
    if (!(value > -k_lattice_limit)) {
        return -k_lattice_limit;
    }
    if (!(value < k_lattice_limit)) {
        return k_lattice_limit;
    }
    return value;
}

float octave_value(uint32_t seed, uint32_t salt, float x, float y) {
    const float cx = clamp_lattice(x);
    const float cy = clamp_lattice(y);
    const float fx = std::floor(cx);
    const float fy = std::floor(cy);
    const int32_t ix = static_cast<int32_t>(fx);
    const int32_t iy = static_cast<int32_t>(fy);
    const float tx = cx - fx;
    const float ty = cy - fy;

    const float n00 = unit_from_bits(lattice_hash(seed, salt, ix, iy));
    const float n10 = unit_from_bits(lattice_hash(seed, salt, ix + 1, iy));
    const float n01 = unit_from_bits(lattice_hash(seed, salt, ix, iy + 1));
    const float n11 = unit_from_bits(lattice_hash(seed, salt, ix + 1, iy + 1));

    const float u = fade5(tx);
    const float v = fade5(ty);
    return lerpf(lerpf(n00, n10, u), lerpf(n01, n11, u), v);
}

} // namespace

void sdc_set_load_layers(int layers) {
    if (layers < k_load_layers_min) {
        layers = k_load_layers_min;
    }
    if (layers > k_load_layers_max) {
        layers = k_load_layers_max;
    }
    g_layers = layers;
}

int sdc_load_layers() {
    return g_layers;
}

float sdc_load_sample(uint32_t seed, double world_x, double world_y) {
    // The only place double is needed: the world coordinate can be large, and
    // it should be narrowed to float only after the division by the period.
    const float base_x = static_cast<float>(world_x * (1.0 / 64.0));
    const float base_y = static_cast<float>(world_y * (1.0 / 64.0));

    const int layers = g_layers;

    float sum = 0.0f;
    float weight_sum = 0.0f;

    for (int layer = 0; layer < layers; ++layer) {
        // Every layer starts from the base coordinate with its own scale and
        // salt. It is that restart, rather than multiplying the frequency
        // straight through, that keeps the coordinate bounded for any layer
        // count.
        const uint32_t salt = 0x9E3779B9u + static_cast<uint32_t>(layer) * 0x85EBCA6Bu;
        const float layer_scale = 1.0f + static_cast<float>(layer) * 0.37f;

        float x = base_x * layer_scale;
        float y = base_y * layer_scale;
        float amplitude = 1.0f;

        for (int octave = 0; octave < k_load_octaves_per_layer; ++octave) {
            sum += amplitude * octave_value(seed, salt + static_cast<uint32_t>(octave), x, y);
            weight_sum += amplitude;

            // Rotation between octaves by an angle that is not a multiple of
            // the lattice symmetry. A rational rotation would map the integer
            // lattice onto itself, so octave nodes would keep coinciding. This
            // does not change the cost; it avoids a degenerate pattern where
            // all four cell corners often coincide and the hashes get served
            // from cache.
            const float rx = x * 0.564634886f - y * 0.825340805f;
            const float ry = x * 0.825340805f + y * 0.564634886f;
            x = rx * 2.0f;
            y = ry * 2.0f;
            amplitude *= 0.5f;
        }
    }

    if (!(weight_sum > 0.0f)) {
        return 0.0f;
    }
    // The scale into metres is arbitrary. All that matters is that the bytes
    // of the result are varied and non-zero: a value made entirely of zeroes
    // would make a "read 00" fault indistinguishable from a correct read.
    return (sum / weight_sum) * 400.0f;
}
