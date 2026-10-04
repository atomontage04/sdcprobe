// Reproducer of a CPU fault: inside a function that addresses its locals through rbx, rbx takes
// the value rsp had before the `sub $N,%rsp` of the prologue, although no instruction writes it.
// See README.md.
//
// The program is an FNV-1a hash over vectors of 16-bit nodes, fed two bytes at a time through
// emit_i16 -> lambda. Built at -O0 with AddressSanitizer, emit_i16 gets the prologue
//   push %rbp; mov %rsp,%rbp; push %rbx; and $-32,%rsp; sub $0xa0,%rsp; mov %rsp,%rbx
// and one call of __asan_stack_malloc_0 per invocation; that shape is what fails.
// A pass hashes `chunks` vectors of 4096 nodes; every pass must give the hash of the first one.
//
// build: clang++ -g -O0 -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all
//        -fno-omit-frame-pointer -o repro repro.cpp
// usage: repro <seconds> [chunks]
// exit:  0 clean; non-zero is a fault: either the line MISMATCH (a hash differs) or, as in every
//        case seen so far, a report of the sanitizer about a signal
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr std::uint64_t kFnvOffsetBasis64 = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime64 = 1099511628211ULL;

struct Chunk {
    double edited_at_s = 0.0;
    std::vector<std::int16_t> nodes;
};

template <class Emit>
void emit_u64(Emit& emit, std::uint64_t value) {
    unsigned char buffer[8];
    for (int i = 0; i < 8; ++i) buffer[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFU);
    emit(buffer, sizeof buffer);
}

// The function that fails. The local array makes ASan give it a fake-stack frame with a dynamic
// alloca as the fallback, and the dynamic alloca makes the compiler keep the frame base in rbx.
template <class Emit>
void emit_i16(Emit& emit, std::int16_t value) {
    const std::uint16_t bits = static_cast<std::uint16_t>(value);
    unsigned char buffer[2];
    buffer[0] = static_cast<unsigned char>(bits & 0xFFU);
    buffer[1] = static_cast<unsigned char>((bits >> 8) & 0xFFU);
    emit(buffer, sizeof buffer);
}

template <class Emit>
void emit_state(const std::vector<Chunk>& chunks, Emit& emit) {
    emit_u64(emit, static_cast<std::uint64_t>(chunks.size()));
    for (const Chunk& chunk : chunks) {
        emit_u64(emit, static_cast<std::uint64_t>(chunk.nodes.size()));
        for (const std::int16_t node : chunk.nodes) emit_i16(emit, node);
    }
}

std::uint64_t hash(const std::vector<Chunk>& chunks) noexcept {
    std::uint64_t value = kFnvOffsetBasis64;
    auto emit = [&value](const unsigned char* bytes, std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            value ^= static_cast<std::uint64_t>(bytes[i]);
            value *= kFnvPrime64;
        }
    };
    emit_state(chunks, emit);
    return value;
}

} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10.0;
    const std::size_t count = argc > 2 ? static_cast<std::size_t>(std::atol(argv[2])) : 64;
    std::vector<Chunk> chunks(count);
    std::uint32_t seed = 12345;
    for (Chunk& chunk : chunks) {
        chunk.nodes.resize(4096);
        for (std::int16_t& node : chunk.nodes) {
            seed = seed * 1664525U + 1013904223U;
            node = static_cast<std::int16_t>(seed >> 16);
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t expected = hash(chunks);
    unsigned long passes = 1;
    for (;;) {
        const std::uint64_t got = hash(chunks);
        ++passes;
        if (got != expected) {
            std::printf("MISMATCH pass %lu expected %016llx got %016llx\n", passes,
                        static_cast<unsigned long long>(expected), static_cast<unsigned long long>(got));
            return 1;
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed.count() >= seconds) break;
    }
    std::printf("CLEAN passes %lu\n", passes);
    return 0;
}
