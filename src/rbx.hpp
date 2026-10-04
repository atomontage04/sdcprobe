#pragma once

// The rbx probe: the second fault this tool looks for.
//
// What it looks for
// -----------------
// A function that keeps its frame base in rbx — which is what a compiler emits
// when a frame is both over-aligned and dynamically sized —
//
//     push %rbp ; mov %rsp,%rbp ; push %rbx
//     and  $-32,%rsp
//     sub  $0xa0,%rsp
//     mov  %rsp,%rbx            every local is addressed through rbx from here
//     call ...                  the callee does push %rbx ... pop %rbx
//     mov  0x80(%rbx),%rcx
//
// finds, somewhere between its prologue and its epilogue, that rbx holds
// rsp + 0xa0: the value rsp had BEFORE the `sub`. No instruction writes that
// value into rbx, and it exists nowhere in memory. The function then reads its
// locals 0xa0 bytes too high, out of its caller's frame, and whatever happens
// next — a wild pointer, a wrong return address — is a consequence.
//
// Why the code is carried as bytes
// --------------------------------
// The fault was found in a build of a small hash loop at -O0 under
// AddressSanitizer, and it is tied to that machine code: a hand-written
// imitation of the same prologue, the same call and the same push/pop produced
// nothing in 10^10 calls while the original was failing next to it. So this
// probe does not imitate. rbx_payload.S holds the original functions, and the
// part of the ASan runtime they call on every iteration, as the bytes of the
// binary that failed. Linux and Windows execute the same bytes.
//
// What that code needs around it, and gets here
// ---------------------------------------------
//   * shadow memory at the address ASan computes with a constant offset,
//     (address >> 3) + 0x7fff8000. Everything the payload touches is therefore
//     placed in one arena at a fixed address, with its shadow mapped at the
//     matching fixed address;
//   * a "fake stack" — the ASan structure the per-call frame comes from — at
//     the pointer the payload reads through %gs:0x28;
//   * the data it hashes, laid out as the std::vector objects it was compiled
//     against.
//
// How a fault shows
// -----------------
// Not as a wrong hash: in every observed case the function went off through a
// garbage pointer first. So the detection is a hardware exception inside the
// payload. It is caught, the registers are recorded, the arena is rebuilt and
// the run goes on. The signature is in the registers: rbx - rsp == 0xa0 at an
// instruction of emit_i16 where the two must be equal.

#include "platform.hpp"

#include <cstdint>
#include <string>

// emit_i16 calls per pass: 64 chunks of 4096 nodes. Each call is one exposure
// of the frame base to the fault.
inline constexpr uint64_t k_rbx_calls_per_pass = 64ull * 4096ull;

// The displacement of the `sub` in emit_i16's prologue, and therefore the value
// of rbx - rsp that identifies the fault.
inline constexpr uint64_t k_rbx_signature_delta = 0xa0ull;

// Whether this CPU can execute the payload at all: it contains two VEX-encoded
// instructions (from memset), one of them AVX2. On false, `reason` says what is
// missing.
bool sdc_rbx_supported(std::string& reason);

// Maps the arena and its shadow, builds the data, installs fault capture.
// On false, `error` is fit to show a human.
bool sdc_rbx_prepare(std::string& error);

struct RbxPass {
    bool faulted = false;  // a hardware exception was raised inside the payload
    bool mismatch = false; // the pass completed and returned the wrong hash
    uint64_t hash = 0u;
    uint64_t expected = 0u;
    FaultRecord fault;
};

// One hash pass through the payload. After a faulted pass the arena has been
// rebuilt from scratch: nothing the broken call wrote survives into the next.
RbxPass sdc_rbx_run_pass();

// "emit_i16+0x229" for an address inside the payload, with the address the
// same instruction had in the original binary; a plain hex number otherwise.
std::string sdc_rbx_describe_address(uint64_t address);

// True if the registers carry the signature: inside emit_i16, rbx ahead of rsp
// by exactly the frame size.
bool sdc_rbx_is_signature(const FaultRecord& fault);

// Checks the machinery on healthy hardware: a clean pass must hash correctly,
// and a deliberately corrupted frame base must be caught and recovered from.
// See the definition.
struct RbxSelfTest {
    bool clean_pass_ok = false;
    bool injected_caught = false;
    bool recovered = false;
    std::string detail;
};
RbxSelfTest sdc_rbx_self_test();
