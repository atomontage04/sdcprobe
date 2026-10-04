#pragma once

// Thin platform layer: enumerating logical processors, pinning to one of them,
// telling whether input is interactive, and installing the interrupt handler.
//
// Everything that differs between Linux and Windows lives here and only here.
// sdcprobe.cpp contains no platform conditionals at all — otherwise the main
// flow of the program would have to be read in two variants at once.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One logical processor.
//
// index is the running number the user types and the report prints. On Linux it
// equals the system CPU id, which is also the taskset argument. On Windows the
// numbering runs across all processor groups; with a single group, which is any
// ordinary machine, it also equals the bit position in an /affinity mask.
struct CpuInfo {
    uint32_t index = 0u;
    uint16_t group = 0u;    // Windows processor group; always 0 on Linux
    uint32_t in_group = 0u; // index within the group; the CPU id on Linux
};

// Logical processors available to THIS process.
//
// Available, not merely present: under taskset, in a container with a
// restricted cpuset, or with an affinity mask already applied, the tool must
// show what it can actually use. Otherwise pinning fails later, in the middle
// of a run.
std::vector<CpuInfo> sdc_enumerate_cpus();

// Pins the CURRENT thread to one logical processor.
// false means pinning failed; the caller must notice, because an unpinned run
// does not measure what the report claims it measured.
bool sdc_pin_to_cpu(const CpuInfo& cpu);

// Confirms that pinning actually took effect: the system is free to narrow or
// ignore the request. An empty string means all good, otherwise it describes
// the mismatch.
std::string sdc_verify_pin(const CpuInfo& cpu);

// Whether the program is reading from a terminal. If not (redirected input, a
// CI job, a double-clicked .exe with no console), it must not ask questions:
// it takes the defaults and says so.
bool sdc_stdin_is_tty();

// Installs a handler for interactive interrupts (Ctrl+C, and console close on
// Windows). A full sweep of every core takes hours, so an interrupt has to end
// with a partial report rather than nothing at all.
void sdc_install_interrupt_handler();

// True once an interrupt has been requested. Polled from the measurement loop.
bool sdc_interrupt_requested();

const char* sdc_platform_name();

// ---------------------------------------------------------------------------
// Services for the rbx probe. Its payload is foreign machine code with fixed
// expectations about its surroundings (see rbx.hpp); these three are the parts
// of those surroundings that only the operating system can provide.
// ---------------------------------------------------------------------------

// Maps `size` bytes of zero-filled read-write memory at exactly `address`.
// nullptr if that range is not free. Never moves the mapping elsewhere: the
// payload computes addresses with a constant offset, so "somewhere else" is
// the same as "not at all".
void* sdc_map_fixed(uint64_t address, size_t size);
void sdc_unmap(void* base, size_t size);

// The payload reads its thread-local pointer with `mov %gs:0x28,%rax`. Between
// enter and leave that read returns `value` on the calling thread.
// On Windows the slot is TEB.ArbitraryUserPointer and leave puts back what was
// there; on Linux the gs base is otherwise unused in a 64-bit process.
bool sdc_payload_tp_enter(void* value);
void sdc_payload_tp_leave();

// Registers of the thread at the moment of a hardware exception.
struct FaultRecord {
    const char* what = "";  // "access violation", "illegal instruction", ...
    uint64_t address = 0u;  // faulting data address, where the system reports one
    uint64_t rip = 0u;
    uint64_t rsp = 0u;
    uint64_t rbp = 0u;
    uint64_t rbx = 0u;
    uint64_t rax = 0u;
    uint64_t rcx = 0u;
    uint64_t rdx = 0u;
    uint64_t rsi = 0u;
    uint64_t rdi = 0u;
};

// Catches hardware exceptions raised by the payload instead of letting them
// kill the process: the process is the thing writing the report.
//
// While armed, an exception on this thread fills `record` and resumes execution
// at `resume_at`, which is responsible for restoring a sane stack pointer.
// While disarmed the handlers stand aside and a crash is a crash.
bool sdc_fault_capture_install();
void sdc_fault_capture_arm(FaultRecord* record, const void* resume_at);
void sdc_fault_capture_disarm();
