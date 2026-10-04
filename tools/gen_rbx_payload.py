#!/usr/bin/env python3
"""Regenerates src/rbx_payload.S from a build of rbx-repro/repro.cpp.

Why this exists
---------------
The rbx fault (see README, "The second probe") does not reproduce on a
hand-written imitation of the code it was found in. It reproduces on the exact
machine code clang 21 emits for repro.cpp at -O0 with AddressSanitizer, together
with the piece of the ASan runtime that code calls on every iteration. Neither
can be rebuilt by the compilers sdcprobe is built with, and the ASan runtime
does not exist for MinGW at all.

So the machine code is carried as bytes. This script takes the linked repro
binary - the very binary that faults - walks the call graph from hash(), and
writes every function it reaches as `.byte` lines, instruction by instruction,
with the disassembly as a comment. Only three kinds of bytes are not copied
verbatim:

  * rel32 displacements that leave a function (calls, rip-relative data) become
    assembler expressions, because the functions are laid out afresh;
  * the read of the thread pointer, `mov %fs:0,%rax`, becomes
    `mov %gs:0x28,%rax`: %fs is not available on Windows, %gs with a
    caller-provided base works on both systems, and the result is that Linux
    and Windows execute the same bytes;
  * everything the hot path never reaches (sanitizer report functions, the
    rest of the runtime) is replaced by `ud2` stubs, one per target, so that
    arriving there is caught and named.

Each function keeps its original address modulo 4096.

The one piece that is not transplanted is memset. The ASan runtime ends every
fake-frame allocation with a zero-length memset through a function pointer,
which in the faulting process was glibc's AVX2 variant. Its zero-length path is
fourteen instructions; sdc_rbx_memset below reproduces that path at the same
offsets and handles every other length with `rep stosb`.

Usage
-----
    clang++ -g -O0 -std=c++20 -fsanitize=address,undefined \\
        -fno-sanitize-recover=all -fno-omit-frame-pointer \\
        -o rbx-repro/repro rbx-repro/repro.cpp
    clang++ <same flags> -c -o rbx-repro/repro.o rbx-repro/repro.cpp
    tools/gen_rbx_payload.py rbx-repro/repro rbx-repro/repro.o \
        > src/rbx_payload.S

The object file is only read for its symbol table: it says which functions of
the binary came from repro.cpp and which from the runtime.

The output depends on the exact compiler and runtime. The committed file was
produced with Ubuntu clang 21.1.8; a different version will produce a different
payload, which may or may not still provoke the fault. That is why the result
is committed instead of being generated during the build.
"""

import re
import subprocess
import sys

ENTRY_PATTERN = re.compile(r"^\(anonymous namespace\)::hash\(std::vector<.*> const&\)$")

# Runtime functions that are part of the hot path and are therefore copied.
RUNTIME_COPIED = {
    "__asan_stack_malloc_0",
    "__asan_stack_malloc_1",
    "__asan_stack_malloc_2",
    "__asan_stack_malloc_3",
    "__asan_set_shadow_f5",
    "_ZN6__asan12PoisonShadowEmmh",
}

# Runtime data the copied code reads, with the values it had in the faulting
# process. Everything else it can reach is on a path that ends in a trap stub.
DATA_SPECIAL = {
    "__asan_option_detect_stack_use_after_return": "sdc_rbx_d_detect_uar",
    "_ZN6__asan10kMidMemBegE": "sdc_rbx_d_mid_beg",
    "_ZN6__asan10kMidMemEndE": "sdc_rbx_d_mid_end",
    "_ZN6__asan11kHighMemEndE": "sdc_rbx_d_high_end",
    "_ZN6__asanL17can_poison_memoryE": "sdc_rbx_d_can_poison",
    "_ZN14__interception11real_memsetE": "sdc_rbx_d_real_memset",
}

# Runtime data that only paths ending in a trap stub read. References to these,
# and to anonymous read-only data (strings and descriptors passed to report
# functions), are pointed at a block of zeroes. A reference to any OTHER named
# object stops the generator: guessing its value is how a payload ends up
# quietly taking a different path from the original.
DATA_COLD = {
    "_ZN6__asan28asan_flags_dont_use_directlyE",
    "_ZN11__sanitizer21common_flags_dont_useE",
    "_ZN11__sanitizerL14PageSizeCachedE",
    "_ZN11__sanitizer14PageSizeCachedE",
}

TLS_READ_FS = bytes.fromhex("66666664488b042500000000")
TLS_READ_GS = bytes.fromhex("66666665488b042528000000")
TLS_SLOT_DISP = bytes.fromhex("b8ffffff")  # -0x48, little-endian

PAGE = 4096


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def strip_groups(text, open_ch, close_ch):
    out = []
    depth = 0
    for ch in text:
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def short_name(demangled):
    if "::$_0::operator()" in demangled:
        return "hash::lambda"
    text = demangled.replace("(anonymous namespace)::", "")
    text = strip_groups(text, "<", ">")
    text = strip_groups(text, "(", ")")
    text = text.replace("__asan::", "asan::")
    # What is left is "[return type] qualified::name [const]".
    words = [w for w in text.split() if w != "const"]
    return words[-1] if words else demangled


class Symbol:
    def __init__(self, addr, size, kind, name):
        self.addr = addr
        self.size = size
        self.kind = kind
        self.name = name
        self.demangled = name


def load_symbols(binary):
    symbols = []
    raw = run("nm", "-S", "--defined-only", binary).splitlines()
    nice = run("nm", "-S", "--defined-only", "-C", binary).splitlines()
    for line, nice_line in zip(raw, nice):
        parts = line.split(None, 3)
        if len(parts) != 4:
            continue
        symbol = Symbol(int(parts[0], 16), int(parts[1], 16), parts[2], parts[3])
        symbol.demangled = nice_line.split(None, 3)[3]
        symbols.append(symbol)
    return symbols


def load_unit_functions(source_object):
    names = set()
    for line in run("nm", "--defined-only", source_object).splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            names.add(parts[2])
    return names


INSN = re.compile(r"^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*(?:\t(.*))?$")
BRANCH = re.compile(r"^(?:call|jmp|j[a-z]+|bnd jmp|notrack jmp)\s+([0-9a-f]+) <([^>]*)>")
RIPREL = re.compile(r"# ([0-9a-f]+) <([^>]*)>")


def load_instructions(binary):
    insns = {}
    for line in run("objdump", "-d", "-w", binary).splitlines():
        match = INSN.match(line)
        if match:
            addr = int(match.group(1), 16)
            insns[addr] = (bytes.fromhex(match.group(2).replace(" ", "")), match.group(3) or "")
    return insns


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen_rbx_payload.py <repro binary> <repro object>")
    binary, source_object = sys.argv[1], sys.argv[2]

    symbols = load_symbols(binary)
    functions = sorted((s for s in symbols if s.kind in "tTwW" and s.size), key=lambda s: s.addr)
    objects = [s for s in symbols if s.kind in "bBdDrRvV" and s.size]
    by_addr = {}
    for f in functions:
        by_addr.setdefault(f.addr, f)
    unit_names = load_unit_functions(source_object)
    insns = load_instructions(binary)

    def function_at(addr):
        for f in functions:
            if f.addr <= addr < f.addr + f.size:
                return f
        return None

    def object_at(addr):
        for o in objects:
            if o.addr <= addr < o.addr + o.size:
                return o
        return None

    entry = [f for f in functions if ENTRY_PATTERN.match(f.demangled)]
    if len(entry) != 1:
        sys.exit("cannot identify hash() in %s" % binary)
    entry = entry[0]

    def copied(f):
        return f.name in unit_names or f.name in RUNTIME_COPIED

    # Call-graph closure from hash().
    chosen = {}
    traps = {}
    work = [entry]
    body = {}
    while work:
        f = work.pop()
        if f.addr in chosen:
            continue
        chosen[f.addr] = f
        lines = []
        addr = f.addr
        end = f.addr + f.size
        while addr < end:
            if addr not in insns:
                sys.exit("no instruction at %x in %s" % (addr, f.demangled))
            raw, text = insns[addr]
            lines.append((addr, raw, text))
            addr += len(raw)
        body[f.addr] = lines
        for addr, raw, text in lines:
            match = BRANCH.match(text)
            if not match:
                continue
            target = int(match.group(1), 16)
            if f.addr <= target < end:
                continue
            callee = by_addr.get(target)
            if callee is None:
                # No symbol of its own: a PLT stub. objdump names those.
                traps.setdefault(target, match.group(2))
            elif copied(callee):
                work.append(callee)
            else:
                traps.setdefault(target, callee.name)

    label = {}
    for index, addr in enumerate(sorted(chosen)):
        label[addr] = "sdc_rbx_fn_%d" % index
    trap_label = {}
    for index, addr in enumerate(sorted(traps)):
        trap_label[addr] = "sdc_rbx_trap_%d" % index

    def code_ref(target):
        """Assembler symbol for a code address outside the current function."""
        if target in label:
            return label[target]
        if target in trap_label:
            return trap_label[target]
        inside = function_at(target)
        if inside is not None and inside.addr in label:
            return "%s+%d" % (label[inside.addr], target - inside.addr)
        return None

    def data_ref(target, shown):
        obj = object_at(target)
        if obj is not None and obj.name in DATA_SPECIAL:
            return "%s+%d" % (DATA_SPECIAL[obj.name], target - obj.addr)
        if shown in DATA_COLD or "+0x" in shown:
            return "sdc_rbx_d_dummy"
        sys.exit("reference to runtime data with no known value: %s" % shown)

    out = []
    emit = out.append

    def emit_bytes(raw, comment):
        emit("    .byte %-48s # %s" % (",".join("0x%02x" % b for b in raw), comment))

    def emit_rel32(raw, addr, target, symbol, comment):
        disp = (target - (addr + len(raw))) & 0xFFFFFFFF
        pattern = disp.to_bytes(4, "little")
        pos = raw.rfind(pattern)
        if pos < 0 or raw.count(pattern) != 1:
            sys.exit("cannot locate rel32 in %s at %x" % (raw.hex(), addr))
        tail = len(raw) - pos
        emit("    .byte %-48s # %s" % (",".join("0x%02x" % b for b in raw[:pos]), comment))
        emit("    .long %s - . - %d" % (symbol, tail))
        if tail > 4:
            emit("    .byte %s" % ",".join("0x%02x" % b for b in raw[pos + 4:]))

    emit(HEADER.rstrip("\n"))
    emit("")
    emit("    call   %s" % label[entry.addr])
    emit(AFTER_CALL.rstrip("\n"))

    emit("")
    emit("# ---------------------------------------------------------------------------")
    emit("# Trap stubs: one per function the payload could call but never does on a")
    emit("# healthy machine. Arriving at one raises an invalid-opcode exception whose")
    emit("# address identifies the stub.")
    emit("# ---------------------------------------------------------------------------")
    for addr in sorted(traps):
        emit("    .p2align 3, 0xcc")
        emit("%s:" % trap_label[addr])
        emit("    ud2")
    emit("")

    # Layout: every function keeps its original address modulo the page size.
    emit("# ---------------------------------------------------------------------------")
    emit("# Transplanted functions")
    emit("# ---------------------------------------------------------------------------")
    emit("    .p2align 12, 0xcc")
    cursor = 0
    remaining = sorted(chosen.values(), key=lambda f: (f.addr % PAGE, f.addr))
    order = []
    while remaining:
        want = cursor % PAGE
        pick = next((f for f in remaining if f.addr % PAGE >= want), remaining[0])
        remaining.remove(pick)
        pad = (pick.addr - cursor) % PAGE
        order.append((pick, pad))
        cursor += pad + pick.size

    tls_reads = 0
    for f, pad in order:
        lines = body[f.addr]
        end = f.addr + f.size
        emit("")
        if pad:
            emit("    .skip %d, 0xcc" % pad)
        emit("# %s" % f.demangled)
        emit("# original address 0x%x, %d bytes" % (f.addr, f.size))
        is_emit_i16 = "emit_i16<" in f.demangled
        if is_emit_i16:
            emit("    .globl sdc_rbx_emit_i16")
            emit("sdc_rbx_emit_i16:")
        emit("%s:" % label[f.addr])
        expect_tls_slot = False
        for addr, raw, text in lines:
            comment = "%6x: %s" % (addr, " ".join(text.split()))
            if raw == TLS_READ_FS:
                emit_bytes(TLS_READ_GS, comment + "   [replaced: mov %gs:0x28,%rax]")
                expect_tls_slot = True
                tls_reads += 1
                continue
            if expect_tls_slot:
                if not raw.endswith(TLS_SLOT_DISP):
                    sys.exit("unexpected instruction after the thread pointer read at %x" % addr)
                expect_tls_slot = False
            branch = BRANCH.match(text)
            if branch:
                target = int(branch.group(1), 16)
                if f.addr <= target < end:
                    emit_bytes(raw, comment)
                else:
                    symbol = code_ref(target)
                    if symbol is None:
                        sys.exit("unresolved branch target %x at %x" % (target, addr))
                    emit_rel32(raw, addr, target, symbol, comment)
                continue
            riprel = RIPREL.search(text) if "(%rip)" in text else None
            if riprel:
                target = int(riprel.group(1), 16)
                if f.addr <= target < end:
                    emit_bytes(raw, comment)
                else:
                    symbol = code_ref(target) or data_ref(target, riprel.group(2))
                    emit_rel32(raw, addr, target, symbol, comment)
                continue
            emit_bytes(raw, comment)
        if is_emit_i16:
            emit("    .globl sdc_rbx_emit_i16_end")
            emit("sdc_rbx_emit_i16_end:")
    if tls_reads == 0:
        sys.exit("no thread pointer read found: this runtime is laid out differently")

    emit("")
    emit("# ---------------------------------------------------------------------------")
    emit("# Function table for reports: start, size, original address, name.")
    emit("# ---------------------------------------------------------------------------")
    emit("    .data")
    emit("    .p2align 4")
    emit("    .globl sdc_rbx_functions")
    emit("sdc_rbx_functions:")
    names = []

    def table_entry(symbol, size, original, name):
        names.append(name)
        emit("    .quad %s, %d, 0x%x, .Lname_%d" % (symbol, size, original, len(names) - 1))

    table_entry("sdc_rbx_memset", 0x140, 0, "memset")
    for f, _ in order:
        table_entry(label[f.addr], f.size, f.addr, short_name(f.demangled))
    for addr in sorted(traps):
        symbol = by_addr.get(addr)
        nice = short_name(symbol.demangled) if symbol else traps[addr]
        nice = nice.replace("@plt", "")
        table_entry(trap_label[addr], 2, addr, "trap:" + nice)
    emit("    .quad 0, 0, 0, 0")
    for index, name in enumerate(names):
        emit('.Lname_%d: .asciz "%s"' % (index, name.replace("\\", "\\\\").replace('"', '\\"')))
    emit("")
    emit(FOOTER.rstrip("\n"))

    sys.stdout.write("\n".join(out) + "\n")
    sys.stderr.write("functions %d, traps %d, code bytes %d, thread pointer reads %d\n" %
                     (len(chosen), len(traps), cursor, tls_reads))


HEADER = r"""
// GENERATED by tools/gen_rbx_payload.py from rbx-repro/repro.cpp built
// with Ubuntu clang 21.1.8. Do not edit: the bytes are the measurement.
//
// This is the machine code of the second probe. The functions below are the
// ones a build of repro.cpp at -O0 with AddressSanitizer executes for one hash
// pass, copied instruction by instruction out of the binary that exhibits the
// fault, plus the part of the ASan runtime called on every iteration. See
// tools/gen_rbx_payload.py for what is and is not verbatim, and src/rbx.cpp
// for the environment this code expects.
//
// All code here uses the System V calling convention on every platform.

#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif

    .text
    .p2align 12, 0xcc

// ---------------------------------------------------------------------------
// memset(rdi, esi, rdx). Reached through sdc_rbx_d_real_memset, on the hot
// path always with a length of zero. That path - entry, the jump to +0xe0, the
// compare ladder, ret - matches glibc's AVX2 memset instruction for
// instruction and offset for offset; other lengths take `rep stosb`.
// ---------------------------------------------------------------------------
    .skip 0x400, 0xcc
    .globl sdc_rbx_memset
sdc_rbx_memset:
    .byte 0xf3,0x0f,0x1e,0xfa                # endbr64
    .byte 0xc5,0xf9,0x6e,0xc6                # vmovd  %esi,%xmm0
    .byte 0x48,0x89,0xf8                     # mov    %rdi,%rax
    .byte 0x48,0x83,0xfa,0x20                # cmp    $0x20,%rdx
    .byte 0x0f,0x82                          # jb     sdc_rbx_memset+0xe0
    .long .Lmemset_small - . - 4
.Lmemset_stos:
    mov    %rdi,%r8
    movzbl %sil,%eax
    mov    %rdx,%rcx
    rep stosb
    mov    %r8,%rax
    ret
    .skip 0xe0 - (. - sdc_rbx_memset), 0xcc
.Lmemset_small:
    .byte 0xc4,0xe2,0x79,0x78,0xc0           # vpbroadcastb %xmm0,%xmm0
    .byte 0x83,0xfa,0x10                     # cmp    $0x10,%edx
    .byte 0x7d,0x16                          # jge    +0x100
    .byte 0x83,0xfa,0x08                     # cmp    $0x8,%edx
    .byte 0x7d,0x21                          # jge    +0x110
    .byte 0x83,0xfa,0x04                     # cmp    $0x4,%edx
    .byte 0x7d,0x2c                          # jge    +0x120
    .byte 0x83,0xfa,0x01                     # cmp    $0x1,%edx
    .byte 0x7f,0x37                          # jg     +0x130
    .byte 0x7c,0x03                          # jl     +0xfe
    .byte 0x40,0x88,0x37                     # mov    %sil,(%rdi)
    .byte 0xc3                               # ret
    .byte 0x90
    jmp    .Lmemset_stos                     # +0x100
    .skip 0x110 - (. - sdc_rbx_memset), 0xcc
    jmp    .Lmemset_stos                     # +0x110
    .skip 0x120 - (. - sdc_rbx_memset), 0xcc
    jmp    .Lmemset_stos                     # +0x120
    .skip 0x130 - (. - sdc_rbx_memset), 0xcc
    jmp    .Lmemset_stos                     # +0x130

// ---------------------------------------------------------------------------
// Self-test only. A memset that, on the call numbered by
// sdc_rbx_inject_countdown among those made on behalf of emit_i16, adds 0xa0
// to the rbx that __asan_stack_malloc_0 is about to restore - so emit_i16 gets
// back the frame base the fault would have given it. Installed by swapping
// sdc_rbx_d_real_memset; never reachable otherwise.
//
// Stack on entry: return address into __asan_stack_malloc_0, its alignment
// slot, then its saved rbx, r14, r15, rbp, then the return address into its
// caller.
// ---------------------------------------------------------------------------
    .p2align 6, 0xcc
    .globl sdc_rbx_memset_inject
sdc_rbx_memset_inject:
    mov    0x30(%rsp),%r11
    lea    sdc_rbx_emit_i16(%rip),%r10
    cmp    %r10,%r11
    jb     1f
    lea    sdc_rbx_emit_i16_end(%rip),%r10
    cmp    %r10,%r11
    jae    1f
    subq   $1,sdc_rbx_inject_countdown(%rip)
    jne    1f
    addq   $0xa0,0x10(%rsp)
1:  jmp    sdc_rbx_memset

// ---------------------------------------------------------------------------
// uint64_t sdc_rbx_call(const void* chunks, uint64_t* faulted)
//
// Runs one hash pass. If the fault handler redirects execution to
// sdc_rbx_fault_landing, the stack is rewound to the frame saved here and the
// call returns 0 with *faulted set to 1.
// ---------------------------------------------------------------------------
    .p2align 6, 0xcc
    .globl sdc_rbx_call
sdc_rbx_call:
    push   %rbp
    push   %rbx
    push   %r12
    push   %r13
    push   %r14
    push   %r15
    push   %rsi
    mov    %rsp,sdc_rbx_saved_rsp(%rip)
"""

AFTER_CALL = r"""
    pop    %rsi
    movq   $0,(%rsi)
.Lcall_return:
    pop    %r15
    pop    %r14
    pop    %r13
    pop    %r12
    pop    %rbx
    pop    %rbp
    ret

    .globl sdc_rbx_fault_landing
sdc_rbx_fault_landing:
    mov    sdc_rbx_saved_rsp(%rip),%rsp
    cld
    pop    %rsi
    movq   $1,(%rsi)
    xor    %eax,%eax
    jmp    .Lcall_return
"""

FOOTER = r"""
// ---------------------------------------------------------------------------
// Runtime data read by the transplanted code, with the values it had in the
// process the fault was found in.
// ---------------------------------------------------------------------------
    .p2align 6
sdc_rbx_saved_rsp:      .quad 0
    .globl sdc_rbx_inject_countdown
sdc_rbx_inject_countdown: .quad 0
    .globl sdc_rbx_d_real_memset
sdc_rbx_d_real_memset:  .quad sdc_rbx_memset
sdc_rbx_d_mid_beg:      .quad 0
sdc_rbx_d_mid_end:      .quad 0
sdc_rbx_d_high_end:     .quad 0x00007fffffffffff
sdc_rbx_d_detect_uar:   .long 1
sdc_rbx_d_can_poison:   .byte 1
    .p2align 6
// Everything else the code takes the address of: descriptors handed to report
// functions that are trap stubs here, and frame description strings that are
// stored and never read.
sdc_rbx_d_dummy:        .fill 256, 1, 0
"""

if __name__ == "__main__":
    main()
