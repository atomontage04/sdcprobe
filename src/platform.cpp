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

namespace {

// Fault capture state. One thread, one payload: plain globals are the honest
// representation, and a handler can reach nothing else anyway.
FaultRecord* volatile g_fault_record = nullptr;
const void* volatile g_fault_resume = nullptr;

} // namespace

void sdc_fault_capture_arm(FaultRecord* record, const void* resume_at) {
    g_fault_resume = resume_at;
    g_fault_record = record;
}

void sdc_fault_capture_disarm() {
    g_fault_record = nullptr;
}

#if defined(_WIN32)

#include <io.h>
#include <windows.h>

#include <intrin.h>

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

void* sdc_map_fixed(uint64_t address, size_t size) {
    // VirtualAlloc with an explicit address either gets that address or fails;
    // it rounds down to the 64 KiB allocation granularity, which the callers
    // already respect.
    void* base = VirtualAlloc(reinterpret_cast<void*>(address), size, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (base != reinterpret_cast<void*>(address)) {
        if (base != nullptr) {
            VirtualFree(base, 0, MEM_RELEASE);
        }
        return nullptr;
    }
    return base;
}

void sdc_unmap(void* base, size_t) {
    VirtualFree(base, 0, MEM_RELEASE);
}

namespace {

// Offset of ArbitraryUserPointer in the 64-bit TEB. Documented in the public
// NT_TIB definition, and stable since the first x64 Windows.
constexpr unsigned long k_teb_user_pointer = 0x28ul;

unsigned long long g_saved_user_pointer = 0ull;

const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer divide by zero";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "misaligned access";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error";
    case EXCEPTION_BREAKPOINT:
        return "breakpoint";
    default:
        return "hardware exception";
    }
}

// A vectored handler rather than a frame-based one: vectored handlers run
// before any unwinding is attempted, and the payload has no unwind tables to
// attempt it with.
LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    FaultRecord* record = g_fault_record;
    if (record == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Only errors. Informational and warning codes (debug output, C++ throws
    // have their own severity bits) are not the payload misbehaving.
    if ((code & 0xC0000000ul) != 0xC0000000ul && code != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_fault_record = nullptr;

    CONTEXT* context = info->ContextRecord;
    record->what = exception_name(code);
    record->address = 0u;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        info->ExceptionRecord->NumberParameters >= 2) {
        record->address = info->ExceptionRecord->ExceptionInformation[1];
    }
    record->rip = context->Rip;
    record->rsp = context->Rsp;
    record->rbp = context->Rbp;
    record->rbx = context->Rbx;
    record->rax = context->Rax;
    record->rcx = context->Rcx;
    record->rdx = context->Rdx;
    record->rsi = context->Rsi;
    record->rdi = context->Rdi;

    context->Rip = reinterpret_cast<DWORD64>(g_fault_resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

bool sdc_payload_tp_enter(void* value) {
    g_saved_user_pointer = __readgsqword(k_teb_user_pointer);
    __writegsqword(k_teb_user_pointer, reinterpret_cast<unsigned long long>(value));
    return true;
}

void sdc_payload_tp_leave() {
    __writegsqword(k_teb_user_pointer, g_saved_user_pointer);
}

bool sdc_fault_capture_install() {
    return AddVectoredExceptionHandler(1, on_exception) != nullptr;
}

#elif defined(__linux__)

#include <asm/prctl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
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

void* sdc_map_fixed(uint64_t address, size_t size) {
    // MAP_FIXED_NOREPLACE: plain MAP_FIXED would silently tear down whatever
    // already lives there. On kernels older than 4.17 the flag is ignored and
    // the address becomes a hint, which the comparison below catches.
    void* base = mmap(reinterpret_cast<void*>(address), size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED) {
        return nullptr;
    }
    if (base != reinterpret_cast<void*>(address)) {
        munmap(base, size);
        return nullptr;
    }
    return base;
}

void sdc_unmap(void* base, size_t size) {
    munmap(base, size);
}

namespace {

// What the gs base points at. Only the slot at 0x28 is ever read.
uint64_t g_gs_block[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
bool g_gs_installed = false;

// Signals are delivered on their own stack: a payload that has gone wrong may
// well have a stack pointer nobody should push a signal frame onto.
alignas(16) unsigned char g_signal_stack[64 * 1024];

const char* signal_name(int number) {
    switch (number) {
    case SIGSEGV:
        return "access violation";
    case SIGBUS:
        return "bus error";
    case SIGILL:
        return "illegal instruction";
    case SIGFPE:
        return "arithmetic exception";
    case SIGTRAP:
        return "breakpoint";
    default:
        return "hardware exception";
    }
}

void on_fault(int number, siginfo_t* info, void* raw_context) {
    FaultRecord* record = g_fault_record;
    if (record == nullptr) {
        // Not ours: a genuine crash of the tool. Put the default action back
        // and return; the instruction faults again and the process dies the
        // ordinary way, core dump included.
        std::signal(number, SIG_DFL);
        return;
    }
    g_fault_record = nullptr;

    ucontext_t* context = static_cast<ucontext_t*>(raw_context);
    greg_t* regs = context->uc_mcontext.gregs;
    record->what = signal_name(number);
    record->address = (number == SIGSEGV || number == SIGBUS)
                          ? reinterpret_cast<uint64_t>(info->si_addr)
                          : 0u;
    record->rip = static_cast<uint64_t>(regs[REG_RIP]);
    record->rsp = static_cast<uint64_t>(regs[REG_RSP]);
    record->rbp = static_cast<uint64_t>(regs[REG_RBP]);
    record->rbx = static_cast<uint64_t>(regs[REG_RBX]);
    record->rax = static_cast<uint64_t>(regs[REG_RAX]);
    record->rcx = static_cast<uint64_t>(regs[REG_RCX]);
    record->rdx = static_cast<uint64_t>(regs[REG_RDX]);
    record->rsi = static_cast<uint64_t>(regs[REG_RSI]);
    record->rdi = static_cast<uint64_t>(regs[REG_RDI]);

    // Returning from the handler restores this context, so execution continues
    // at the landing point with the signal mask back to what it was.
    regs[REG_RIP] = static_cast<greg_t>(reinterpret_cast<uint64_t>(g_fault_resume));
}

} // namespace

bool sdc_payload_tp_enter(void* value) {
    g_gs_block[5] = reinterpret_cast<uint64_t>(value);
    if (!g_gs_installed) {
        if (syscall(SYS_arch_prctl, ARCH_SET_GS, g_gs_block) != 0) {
            return false;
        }
        g_gs_installed = true;
    }
    return true;
}

void sdc_payload_tp_leave() {
    // The gs base stays where it is: nothing else in a 64-bit Linux process
    // uses it, and setting it is a system call that has no business running
    // between two passes of a measured loop.
}

bool sdc_fault_capture_install() {
    stack_t stack;
    stack.ss_sp = g_signal_stack;
    stack.ss_size = sizeof(g_signal_stack);
    stack.ss_flags = 0;
    if (sigaltstack(&stack, nullptr) != 0) {
        return false;
    }
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    const int numbers[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (const int number : numbers) {
        if (sigaction(number, &action, nullptr) != 0) {
            return false;
        }
    }
    return true;
}

#else

// Stopping at compile time is deliberate. A silent stub that "pins" nowhere
// would produce a report full of core numbers that have nothing to do with
// where the work actually ran.
#error "sdcprobe: core enumeration and pinning are implemented for Linux and Windows only"

#endif
