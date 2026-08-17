#pragma once

// Thin platform layer: enumerating logical processors, pinning to one of them,
// telling whether input is interactive, and installing the interrupt handler.
//
// Everything that differs between Linux and Windows lives here and only here.
// sdcprobe.cpp contains no platform conditionals at all — otherwise the main
// flow of the program would have to be read in two variants at once.

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
