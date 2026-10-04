// The rbx probe. Rationale lives in rbx.hpp.

#include "rbx.hpp"

#include <cstdio>
#include <cstring>

#if !defined(__x86_64__)
#error "the rbx probe is x86-64 machine code"
#endif

// The payload uses the System V calling convention on every platform. Under
// MinGW this attribute makes the compiler bridge the two conventions at the
// call; on Linux it changes nothing.
#define SDC_PAYLOAD_ABI __attribute__((sysv_abi))

extern "C" {

struct RbxFunction {
    const unsigned char* start;
    uint64_t size;
    uint64_t original;
    const char* name;
};

// Everything below is defined in rbx_payload.S.
SDC_PAYLOAD_ABI uint64_t sdc_rbx_call(const void* chunks, uint64_t* faulted);
extern const unsigned char sdc_rbx_fault_landing[];
extern const unsigned char sdc_rbx_emit_i16[];
extern const unsigned char sdc_rbx_emit_i16_end[];
extern const unsigned char sdc_rbx_memset[];
extern const unsigned char sdc_rbx_memset_inject[];
extern const RbxFunction sdc_rbx_functions[];
extern const unsigned char* sdc_rbx_d_real_memset;
extern uint64_t sdc_rbx_inject_countdown;

} // extern "C"

namespace {

// ---------------------------------------------------------------------------
// The arena
// ---------------------------------------------------------------------------

// One fixed address for everything the payload reads or writes other than its
// own stack. Fixed, because the payload turns an address into a shadow address
// with a constant; and high, because the ASan runtime code in the payload
// insists that the address lie in what it calls high memory, above
// 0x10007fff8000. Nothing lives here on either Linux or Windows.
constexpr uint64_t k_arena_base = 0x600000000000ull;

// The fake stack: a header, a flag byte per frame, then eleven size classes of
// 2^20 bytes each. 20 is the value the ASan runtime chose in the process the
// fault was found in (an 8 MiB main stack, clamped by max_uar_stack_size_log).
constexpr uint64_t k_stack_size_log = 20u;
constexpr uint64_t k_fake_stack_bytes = 0x1000ull + (1ull << (k_stack_size_log - 5u)) +
                                        11ull * (1ull << k_stack_size_log);
constexpr uint64_t k_fake_stack_region = 0xc00000ull;
static_assert(k_fake_stack_bytes <= k_fake_stack_region, "the fake stack does not fit");

// Offsets of the two fields of the runtime's FakeStack object that have to be
// initialised; the rest starts as zeroes.
constexpr uint64_t k_fake_stack_size_log_offset = 0x58u;

constexpr uint64_t k_data_base = k_arena_base + k_fake_stack_region;
constexpr uint64_t k_data_region = 0x100000ull;
constexpr uint64_t k_arena_bytes = k_fake_stack_region + k_data_region;

// Shadow of the arena, at the address the payload computes. Mapped from the
// 64 KiB boundary below it, because that is the granularity Windows allocates
// at.
constexpr uint64_t k_shadow_offset = 0x7fff8000ull;
constexpr uint64_t k_shadow_base = (k_arena_base >> 3u) + k_shadow_offset;
constexpr uint64_t k_shadow_map_base = k_shadow_base & ~0xffffull;
constexpr uint64_t k_shadow_map_bytes =
    ((k_shadow_base + (k_arena_bytes >> 3u) + 0xffffull) & ~0xffffull) - k_shadow_map_base;

// The data: std::vector<Chunk>, where Chunk is { double; std::vector<int16_t> }
// and a vector is three pointers. Same counts and same generator as the
// original reproducer.
constexpr uint64_t k_chunk_count = 64u;
constexpr uint64_t k_nodes_per_chunk = 4096u;
static_assert(k_chunk_count * k_nodes_per_chunk == k_rbx_calls_per_pass, "calls per pass");
constexpr uint32_t k_data_seed = 12345u;

constexpr uint64_t k_vector_address = k_data_base + 0x100u;
constexpr uint64_t k_chunks_address = k_data_base + 0x1000u;
constexpr uint64_t k_nodes_address = k_data_base + 0x10000u;
constexpr uint64_t k_nodes_stride = 0x2800u;
static_assert(k_nodes_stride >= k_nodes_per_chunk * sizeof(int16_t), "node arrays overlap");
static_assert(0x10000u + k_chunk_count * k_nodes_stride <= k_data_region, "the data does not fit");

struct VectorImage {
    uint64_t begin;
    uint64_t end;
    uint64_t end_of_storage;
};

struct ChunkImage {
    double edited_at_s;
    VectorImage nodes;
};

static_assert(sizeof(VectorImage) == 24u && sizeof(ChunkImage) == 32u,
              "layout of the std::vector the payload was compiled against");

constexpr uint64_t k_fnv_basis = 14695981039346656037ull;
constexpr uint64_t k_fnv_prime = 1099511628211ull;

bool g_prepared = false;
uint64_t g_expected_hash = 0u;

// What %gs:0x28 points into. The payload reads its fake-stack pointer at -0x48
// from the value it finds there, hence slot 0 for the pointer and slot 9 for
// the value.
uint64_t g_thread_area[16] = {};

void fnv_bytes(uint64_t& hash, uint64_t value, uint32_t count) {
    for (uint32_t i = 0u; i < count; ++i) {
        hash ^= (value >> (8u * i)) & 0xffull;
        hash *= k_fnv_prime;
    }
}

// Puts the arena into the state the payload expects at the start of a pass:
// an untouched fake stack, clean shadow, and the data. Returns the hash a
// correct pass must produce, computed here without the payload.
uint64_t build_arena() {
    std::memset(reinterpret_cast<void*>(k_arena_base), 0, static_cast<size_t>(k_arena_bytes));
    std::memset(reinterpret_cast<void*>(k_shadow_map_base), 0,
                static_cast<size_t>(k_shadow_map_bytes));

    std::memcpy(reinterpret_cast<void*>(k_arena_base + k_fake_stack_size_log_offset),
                &k_stack_size_log, sizeof(k_stack_size_log));

    VectorImage* const vector = reinterpret_cast<VectorImage*>(k_vector_address);
    ChunkImage* const chunks = reinterpret_cast<ChunkImage*>(k_chunks_address);
    vector->begin = k_chunks_address;
    vector->end = k_chunks_address + k_chunk_count * sizeof(ChunkImage);
    vector->end_of_storage = vector->end;

    uint64_t hash = k_fnv_basis;
    fnv_bytes(hash, k_chunk_count, 8u);

    uint32_t seed = k_data_seed;
    for (uint64_t c = 0u; c < k_chunk_count; ++c) {
        const uint64_t nodes_address = k_nodes_address + c * k_nodes_stride;
        int16_t* const nodes = reinterpret_cast<int16_t*>(nodes_address);
        chunks[c].edited_at_s = 0.0;
        chunks[c].nodes.begin = nodes_address;
        chunks[c].nodes.end = nodes_address + k_nodes_per_chunk * sizeof(int16_t);
        chunks[c].nodes.end_of_storage = chunks[c].nodes.end;

        fnv_bytes(hash, k_nodes_per_chunk, 8u);
        for (uint64_t n = 0u; n < k_nodes_per_chunk; ++n) {
            seed = seed * 1664525u + 1013904223u;
            nodes[n] = static_cast<int16_t>(seed >> 16u);
            fnv_bytes(hash, static_cast<uint16_t>(nodes[n]), 2u);
        }
    }

    g_thread_area[0] = k_arena_base;
    return hash;
}

const RbxFunction* find_function(uint64_t address) {
    for (const RbxFunction* f = sdc_rbx_functions; f->start != nullptr; ++f) {
        const uint64_t start = reinterpret_cast<uint64_t>(f->start);
        if (address >= start && address < start + f->size) {
            return f;
        }
    }
    return nullptr;
}

} // namespace

bool sdc_rbx_supported(std::string& reason) {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2")) {
        reason = "this CPU or operating system does not provide AVX2, which the probe's code uses";
        return false;
    }
    return true;
}

bool sdc_rbx_prepare(std::string& error) {
    if (g_prepared) {
        return true;
    }
    if (!sdc_rbx_supported(error)) {
        return false;
    }
    char text[160];
    if (sdc_map_fixed(k_arena_base, static_cast<size_t>(k_arena_bytes)) == nullptr) {
        std::snprintf(text, sizeof(text), "cannot map %llu MiB at address 0x%llx",
                      static_cast<unsigned long long>(k_arena_bytes >> 20u),
                      static_cast<unsigned long long>(k_arena_base));
        error = text;
        return false;
    }
    if (sdc_map_fixed(k_shadow_map_base, static_cast<size_t>(k_shadow_map_bytes)) == nullptr) {
        sdc_unmap(reinterpret_cast<void*>(k_arena_base), static_cast<size_t>(k_arena_bytes));
        std::snprintf(text, sizeof(text), "cannot map %llu KiB at address 0x%llx",
                      static_cast<unsigned long long>(k_shadow_map_bytes >> 10u),
                      static_cast<unsigned long long>(k_shadow_map_base));
        error = text;
        return false;
    }
    if (!sdc_fault_capture_install()) {
        error = "cannot install the hardware exception handler";
        return false;
    }
    g_expected_hash = build_arena();
    g_prepared = true;
    return true;
}

RbxPass sdc_rbx_run_pass() {
    RbxPass pass;
    pass.expected = g_expected_hash;

    uint64_t faulted = 0u;
    if (!sdc_payload_tp_enter(&g_thread_area[9])) {
        // Without the thread pointer the payload would fault on its first
        // call, and that fault would be reported as the CPU's.
        pass.mismatch = true;
        return pass;
    }
    sdc_fault_capture_arm(&pass.fault, sdc_rbx_fault_landing);
    pass.hash = sdc_rbx_call(reinterpret_cast<const void*>(k_vector_address), &faulted);
    sdc_fault_capture_disarm();
    sdc_payload_tp_leave();

    if (faulted != 0u) {
        pass.faulted = true;
        // The broken call has written through pointers it took from the wrong
        // frame. Nothing in the arena can be assumed intact.
        build_arena();
    } else if (pass.hash != pass.expected) {
        pass.mismatch = true;
        build_arena();
    }
    return pass;
}

std::string sdc_rbx_describe_address(uint64_t address) {
    char text[160];
    const RbxFunction* f = find_function(address);
    if (f == nullptr) {
        std::snprintf(text, sizeof(text), "0x%llx (outside the probe's code)",
                      static_cast<unsigned long long>(address));
        return text;
    }
    const uint64_t offset = address - reinterpret_cast<uint64_t>(f->start);
    std::snprintf(text, sizeof(text), "%s+0x%llx", f->name,
                  static_cast<unsigned long long>(offset));
    return text;
}

bool sdc_rbx_is_signature(const FaultRecord& fault) {
    const uint64_t begin = reinterpret_cast<uint64_t>(sdc_rbx_emit_i16);
    const uint64_t end = reinterpret_cast<uint64_t>(sdc_rbx_emit_i16_end);
    return fault.rip >= begin && fault.rip < end &&
           fault.rbx - fault.rsp == k_rbx_signature_delta;
}

// The CH probe can prove its detector by corrupting a byte stream. This probe
// has no stream: its detector is "the payload raised an exception, and here
// are the registers". So the self-test makes the payload raise one, by the
// same means the fault does. A substitute memset, installed for one pass, adds
// 0xa0 to the rbx that is about to be restored into emit_i16 on its 1000th
// call. From there on the real machinery has to do everything it does for a
// real fault: catch the exception, rebuild the arena, and run a correct pass
// afterwards.
//
// The injected change is not expected to end the way the real one does. Here
// rbx is wrong from the first instruction after the call, the function works
// consistently in the wrong frame and returns through a garbage address; the
// real fault strikes later in the function and ends inside it. The signature
// check is therefore not part of this test.
RbxSelfTest sdc_rbx_self_test() {
    RbxSelfTest result;

    const RbxPass clean = sdc_rbx_run_pass();
    result.clean_pass_ok = !clean.faulted && !clean.mismatch;
    if (!result.clean_pass_ok) {
        return result;
    }

    sdc_rbx_inject_countdown = 1000u;
    sdc_rbx_d_real_memset = sdc_rbx_memset_inject;
    const RbxPass injected = sdc_rbx_run_pass();
    sdc_rbx_d_real_memset = sdc_rbx_memset;

    result.injected_caught = injected.faulted;
    if (injected.faulted) {
        result.detail = std::string(injected.fault.what) + " at " +
                        sdc_rbx_describe_address(injected.fault.rip);
    }

    const RbxPass after = sdc_rbx_run_pass();
    result.recovered = !after.faulted && !after.mismatch;
    return result;
}
