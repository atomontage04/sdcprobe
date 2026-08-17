// Platform layer. Rationale lives in platform.hpp.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "platform.hpp"

#include <csignal>
#include <cstdio>

namespace {

// Written from a signal handler, so the type has to be sig_atomic_t and the
// handler has to do nothing but set it. Everything else — printing the partial
// report, closing files — happens on the main path, where it is safe.
volatile std::sig_atomic_t g_interrupt = 0;

void on_interrupt(int) {
    g_interrupt = 1;
}

} // namespace

bool sdc_interrupt_requested() {
    return g_interrupt != 0;
}

#if defined(_WIN32)

#include <io.h>
#include <windows.h>

namespace {

BOOL WINAPI on_console_ctrl(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_interrupt = 1;
        // TRUE means "handled": Windows then lets the program wind down on its
        // own instead of killing it outright. For CTRL_CLOSE_EVENT the grace
        // period is short, but it is enough to flush the report.
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void sdc_install_interrupt_handler() {
    std::signal(SIGINT, on_interrupt);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
}

std::vector<CpuInfo> sdc_enumerate_cpus() {
    std::vector<CpuInfo> cpus;

    const WORD group_count = GetActiveProcessorGroupCount();

    // An ordinary machine has one group. Then the process affinity mask can be
    // queried and exactly what the process is allowed to use can be shown: if
    // the tool was launched under start /affinity, claiming the other cores
    // would be a lie.
    if (group_count <= 1) {
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) != 0 &&
            process_mask != 0) {
            for (uint32_t bit = 0u; bit < sizeof(DWORD_PTR) * 8u; ++bit) {
                if ((process_mask >> bit) & 1u) {
                    cpus.push_back(CpuInfo{bit, 0u, bit});
                }
            }
            return cpus;
        }
    }

    // More than 64 logical processors: the process mask does not apply, it only
    // describes the current group. Enumerate the groups instead.
    uint32_t global_index = 0u;
    for (WORD group = 0u; group < group_count; ++group) {
        const DWORD count = GetActiveProcessorCount(group);
        for (DWORD in_group = 0u; in_group < count; ++in_group) {
            cpus.push_back(CpuInfo{global_index, group, static_cast<uint32_t>(in_group)});
            ++global_index;
        }
    }
    return cpus;
}

bool sdc_pin_to_cpu(const CpuInfo& cpu) {
    // SetThreadGroupAffinity rather than SetThreadAffinityMask: the latter only
    // works within the current group, so on a machine with two groups a request
    // for a processor in the other group would quietly go somewhere else.
    GROUP_AFFINITY affinity;
    ZeroMemory(&affinity, sizeof(affinity));
    affinity.Group = cpu.group;
    affinity.Mask = static_cast<KAFFINITY>(1) << cpu.in_group;
    return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
}

std::string sdc_verify_pin(const CpuInfo& cpu) {
    // Windows does not report "where am I pinned", but it does report which
    // processor the thread is running on right now. After a successful pin that
    // has to match.
    PROCESSOR_NUMBER current;
    ZeroMemory(&current, sizeof(current));
    GetCurrentProcessorNumberEx(&current);
    if (current.Group != cpu.group || static_cast<uint32_t>(current.Number) != cpu.in_group) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "requested group %u cpu %u, but running on group %u cpu %u",
                      static_cast<unsigned>(cpu.group), static_cast<unsigned>(cpu.in_group),
                      static_cast<unsigned>(current.Group), static_cast<unsigned>(current.Number));
        return std::string(text);
    }
    return std::string();
}

bool sdc_stdin_is_tty() {
    return _isatty(_fileno(stdin)) != 0;
}

const char* sdc_platform_name() {
    return "windows";
}

#elif defined(__linux__)

#include <sched.h>
#include <unistd.h>

void sdc_install_interrupt_handler() {
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
}

std::vector<CpuInfo> sdc_enumerate_cpus() {
    std::vector<CpuInfo> cpus;

    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (uint32_t cpu = 0u; cpu < static_cast<uint32_t>(CPU_SETSIZE); ++cpu) {
            if (CPU_ISSET(cpu, &set)) {
                cpus.push_back(CpuInfo{cpu, 0u, cpu});
            }
        }
        if (!cpus.empty()) {
            return cpus;
        }
    }

    // Fallback: the mask could not be read. Take the number of online
    // processors and assume all are available. Worse than the real thing, but
    // better than an empty list.
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    for (long cpu = 0; cpu < online; ++cpu) {
        cpus.push_back(CpuInfo{static_cast<uint32_t>(cpu), 0u, static_cast<uint32_t>(cpu)});
    }
    return cpus;
}

bool sdc_pin_to_cpu(const CpuInfo& cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu.in_group, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

std::string sdc_verify_pin(const CpuInfo& cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return std::string("sched_getaffinity failed");
    }
    if (CPU_COUNT(&set) != 1 || !CPU_ISSET(cpu.in_group, &set)) {
        char text[160];
        std::snprintf(text, sizeof(text), "requested cpu %u, but affinity mask has %d cpu(s)",
                      static_cast<unsigned>(cpu.in_group), CPU_COUNT(&set));
        return std::string(text);
    }
    return std::string();
}

bool sdc_stdin_is_tty() {
    return isatty(fileno(stdin)) != 0;
}

const char* sdc_platform_name() {
    return "linux";
}

#else

// Stopping at compile time is deliberate. A silent stub that "pins" nowhere
// would produce a report full of core numbers that have nothing to do with
// where the work actually ran.
#error "sdcprobe: core enumeration and pinning are implemented for Linux and Windows only"

#endif
