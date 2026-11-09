"""SPU interpreter tests: tiny programs assembled here, run on the real
interpreter, results checked against hand-computed values.

Usage (from dante-port/): python -I tests/spu_tests.py
"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from spu_isa import OPS, BITS  # noqa: E402

OPC = {name: (fmt, opc) for name, fmt, opc in OPS}


def asm(lines):
    """lines: list of (name, *operands) or ("label", name). Branch targets may be label names."""
    labels, pc = {}, 0
    for ln in lines:
        if ln[0] == "label":
            labels[ln[1]] = pc
        else:
            pc += 4
    words, pc = [], 0
    for ln in lines:
        if ln[0] == "label":
            continue
        name, ops = ln[0], list(ln[1:])
        ops = [labels[o] if isinstance(o, str) else o for o in ops]
        fmt, opc = OPC[name]
        sh = 32 - BITS[fmt]
        w = opc << sh
        if name in ("stop", "stopd"):
            w |= ops[0] & 0x3FFF
        elif name in ("rdch", "rchcnt"):
            w |= (ops[1] << 7) | ops[0]
        elif name == "wrch":
            w |= (ops[0] << 7) | ops[1]
        elif fmt == "RRR":
            rt, ra, rb, rc = ops
            w |= (rt << 21) | (rb << 14) | (ra << 7) | rc
        elif fmt == "RR":
            rt, ra, rb = (ops + [0, 0, 0])[:3]
            w |= (rb << 14) | (ra << 7) | rt
        elif fmt == "RI7":
            rt, ra, i = ops
            w |= ((i & 0x7F) << 14) | (ra << 7) | rt
        elif fmt == "RI8":
            rt, ra, i = ops
            w |= ((i & 0xFF) << 14) | (ra << 7) | rt
        elif fmt == "RI10":
            rt, ra, i = ops
            w |= ((i & 0x3FF) << 14) | (ra << 7) | rt
        elif fmt == "RI16":
            rt, i = ops
            if name.startswith("br") and name not in ("bra", "brasl"):
                i = (i - pc) >> 2
            elif name in ("bra", "brasl", "lqa", "stqa"):
                i >>= 2
            w |= ((i & 0xFFFF) << 7) | rt
        elif fmt == "RI18":
            rt, i = ops
            w |= ((i & 0x3FFFF) << 7) | rt
        words.append(w)
        pc += 4
    return words


def f2u(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


EXIT = ("stop", 0x102)
# (name, program, init {reg: [4 BE words]} / "ls": {addr: [words]} / "mem": {addr: [words]},
#  expect {reg: [4 words]} / "ls" / "mem" / "status")
TESTS = [
    ("il negative", [("il", 3, -5), EXIT], {}, {3: [0xFFFFFFFB] * 4}),
    ("ilhu+iohl", [("ilhu", 3, 0x1234), ("iohl", 3, 0x5678), EXIT], {}, {3: [0x12345678] * 4}),
    ("ila 18-bit", [("ila", 3, 0x3FFF0), EXIT], {}, {3: [0x3FFF0] * 4}),
    ("sf = rb - ra", [("il", 3, 10), ("il", 4, 3), ("sf", 5, 3, 4), EXIT], {}, {5: [0xFFFFFFF9] * 4}),
    ("ai/ahi", [("ai", 4, 3, -1), ("ahi", 5, 3, 2), EXIT], {3: [0x00010001] * 4},
     {4: [0x00010000] * 4, 5: [0x00030003] * 4}),
    ("cg / bg", [("cg", 5, 3, 4), ("bg", 6, 3, 4), EXIT],
     {3: [0xFFFFFFFF, 1, 5, 0], 4: [1, 1, 3, 0]}, {5: [1, 0, 0, 0], 6: [0, 1, 0, 1]}),
    ("rotqbyi 4", [("rotqbyi", 5, 3, 4), EXIT], {3: [1, 2, 3, 4]}, {5: [2, 3, 4, 1]}),
    ("shlqbyi 4", [("shlqbyi", 5, 3, 4), EXIT], {3: [1, 2, 3, 4]}, {5: [2, 3, 4, 0]}),
    ("rotqmbyi -4", [("rotqmbyi", 5, 3, -4), EXIT], {3: [1, 2, 3, 4]}, {5: [0, 1, 2, 3]}),
    ("rotqby by reg", [("rotqby", 5, 3, 4), EXIT], {3: [1, 2, 3, 4], 4: [8, 0, 0, 0]}, {5: [3, 4, 1, 2]}),
    ("shlqbii 4 bits", [("shlqbii", 5, 3, 4), EXIT], {3: [0x10000001, 0, 0, 0xF0000000]},
     {5: [0x00000010, 0, 0xF, 0]}),
    ("rotqmbii -4 bits", [("rotqmbii", 5, 3, -4), EXIT], {3: [0x10000001, 0, 0, 0xF]},
     {5: [0x01000000, 0x10000000, 0, 0]}),
    ("shli / rotmi / rotmai", [("shli", 4, 3, 4), ("rotmi", 5, 3, -4), ("rotmai", 6, 3, -4), EXIT],
     {3: [0x80000010] * 4}, {4: [0x00000100] * 4, 5: [0x08000001] * 4, 6: [0xF8000001] * 4}),
    ("rot / roti", [("roti", 5, 3, 8), EXIT], {3: [0x12345678] * 4}, {5: [0x34567812] * 4}),
    ("shufb", [("shufb", 6, 3, 4, 5), EXIT],
     {3: [0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F], 4: [0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F],
      5: [0x1F001303, 0x80C0E005, 0x10101010, 0x0F0E0D0C]},
     {6: [0x1F001303, 0x00FF8005, 0x10101010, 0x0F0E0D0C]}),
    ("cwd insert control", [("cwd", 6, 1, 4), EXIT], {1: [0x3FFF0, 0, 0, 0]},
     {6: [0x10111213, 0x00010203, 0x18191A1B, 0x1C1D1E1F]}),
    ("cbd insert control", [("cbd", 6, 1, 6), EXIT], {1: [0x10, 0, 0, 0]},
     {6: [0x10111213, 0x14150317, 0x18191A1B, 0x1C1D1E1F]}),
    ("fsmbi / gbb", [("fsmbi", 3, 0xA5A5), ("gbb", 4, 3), EXIT], {},
     {3: [0xFF00FF00, 0x00FF00FF, 0xFF00FF00, 0x00FF00FF], 4: [0xA5A5, 0, 0, 0]}),
    ("fsm / gb", [("fsm", 4, 3), ("gb", 5, 4), EXIT], {3: [0x5, 0, 0, 0]},
     {4: [0, 0xFFFFFFFF, 0, 0xFFFFFFFF], 5: [5, 0, 0, 0]}),
    ("selb", [("selb", 6, 3, 4, 5), EXIT], {3: [0xFFFFFFFF] * 4, 4: [0] * 4, 5: [0xFFFF0000, 0, ~0 & 0xFFFFFFFF, 0x0F0F0F0F]},
     {6: [0x0000FFFF, 0xFFFFFFFF, 0, 0xF0F0F0F0]}),
    ("ceq / cgt / clgt", [("ceq", 5, 3, 4), ("cgt", 6, 3, 4), ("clgt", 7, 3, 4), EXIT],
     {3: [1, 0xFFFFFFFF, 5, 2], 4: [1, 1, 3, 7]},
     {5: [0xFFFFFFFF, 0, 0, 0], 6: [0, 0, 0xFFFFFFFF, 0], 7: [0, 0xFFFFFFFF, 0xFFFFFFFF, 0]}),
    ("mpy signed / mpyu / mpyh", [("mpy", 5, 3, 4), ("mpyu", 6, 3, 4), ("mpyh", 7, 3, 4), EXIT],
     {3: [0x0002FFFD] * 4, 4: [0x00000004] * 4},
     {5: [0xFFFFFFF4] * 4, 6: [0x0003FFF4] * 4, 7: [0x00080000] * 4}),
    ("xsbh / xshw / xswd", [("xsbh", 4, 3), ("xshw", 5, 3), ("xswd", 6, 3), EXIT],
     {3: [0x00000080, 0x00008000, 0x80000000, 0x00000001]},
     {4: [0x0000FF80, 0, 0, 0x00000001], 5: [0x00000080, 0xFFFF8000, 0, 1],
      6: [0, 0x00008000, 0, 1]}),
    ("clz / cntb", [("clz", 4, 3), ("cntb", 5, 3), EXIT], {3: [1, 0, 0x80000000, 0xFF0F0301]},
     {4: [31, 32, 0, 0], 5: [0x00000001, 0, 0x01000000, 0x08040201]}),
    ("loop sum 1..10 (brnz)",
     [("il", 3, 0), ("il", 4, 10), ("label", "top"), ("a", 3, 3, 4), ("ai", 4, 4, -1), ("brnz", 4, "top"), EXIT],
     {}, {3: [55] * 4}),
    ("brsl / bi call",
     [("brsl", 0, "fn"), ("ai", 3, 3, 1), EXIT, ("label", "fn"), ("il", 3, 41), ("bi", 0, 0)], {},
     {3: [42] * 4}),
    ("lqd / stqd", [("lqd", 4, 1, 0x10), ("stqd", 4, 1, 0x20), EXIT], {1: [0x100, 0, 0, 0]},
     {4: [0xA, 0xB, 0xC, 0xD], "ls": {0x300: [0xA, 0xB, 0xC, 0xD]}}),
    ("fa / fm / fma", [("fa", 5, 3, 4), ("fm", 6, 3, 4), ("fma", 7, 3, 4, 5), EXIT],
     {3: [f2u(2.0)] * 4, 4: [f2u(3.0)] * 4}, {5: [f2u(5.0)] * 4, 6: [f2u(6.0)] * 4, 7: [f2u(11.0)] * 4}),
    ("cflts scale 0 / 1", [("cflts", 4, 3, 173), ("cflts", 5, 3, 172), EXIT],
     {3: [f2u(1.9), f2u(-1.5), f2u(3e9), f2u(0.5)]},
     {4: [1, 0xFFFFFFFF, 0x7FFFFFFF, 0], 5: [3, 0xFFFFFFFD, 0x7FFFFFFF, 1]}),
    ("csflt scale 0 / 1", [("csflt", 4, 3, 155), ("csflt", 5, 3, 154), EXIT], {3: [7, 0xFFFFFFFE, 0, 1]},
     {4: [f2u(7.0), f2u(-2.0), 0, f2u(1.0)], 5: [f2u(3.5), f2u(-1.0), 0, f2u(0.5)]}),
    ("frest + fi", [("frest", 4, 3), ("fi", 5, 3, 4), EXIT], {3: [f2u(4.0)] * 4}, {5: [f2u(0.25)] * 4}),
    ("orx / sumb", [("orx", 4, 3), EXIT], {3: [1, 2, 4, 8]}, {4: [15, 0, 0, 0]}),
    ("exit status via out mbox", [("il", 3, 7), ("wrch", 28, 3), EXIT], {}, {"status": 7}),
    # DMA: GET 16 bytes from guest 0x30000000 into LS 0x200, add 1, PUT back to 0x30000010
    ("DMA get / put",
     [("ila", 10, 0x200), ("wrch", 16, 10), ("ilhu", 11, 0x3000), ("wrch", 18, 11), ("il", 12, 16), ("wrch", 19, 12),
      ("il", 13, 0), ("wrch", 20, 13), ("il", 14, 0x40), ("wrch", 21, 14),
      ("lqd", 4, 10, 0), ("ai", 4, 4, 1), ("stqd", 4, 10, 0),
      ("ai", 11, 11, 16), ("wrch", 18, 11), ("il", 14, 0x20), ("wrch", 21, 14), EXIT],
     {"mem": {0x30000000: [1, 2, 3, 4]}}, {"mem": {0x30000010: [2, 3, 4, 5]}, "ls": {0x200: [2, 3, 4, 5]}}),
    # GETLLAR, PUTLLC (succeeds), PUTLLC again without reservation (fails)
    ("atomic reservation",
     [("ila", 10, 0x400), ("wrch", 16, 10), ("ilhu", 11, 0x3000), ("wrch", 18, 11),
      ("il", 14, 0xD0), ("wrch", 21, 14), ("rdch", 5, 27),
      ("lqd", 4, 10, 0), ("ai", 4, 4, 100), ("stqd", 4, 10, 0),
      ("il", 14, 0xB4), ("wrch", 21, 14), ("rdch", 6, 27),
      ("wrch", 21, 14), ("rdch", 7, 27), EXIT],
     {"mem": {0x30000000: [1, 2, 3, 4]}},
     {5: [4, 0, 0, 0], 6: [0, 0, 0, 0], 7: [1, 0, 0, 0], "mem": {0x30000000: [101, 102, 103, 104]}}),
]


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    out = ['#include <cstdio>', '#include "spu/spu.h"', "", "int main() {", "\tvm::init();",
           "\tvm::alloc_fixed(vm::Area::User, 0x30000000, 0x10000);", "\tint fails = 0;"]
    for name, prog, init, exp in TESTS:
        words = asm(prog)
        out.append(f'\t{{ // {name}')
        out.append("\t  auto t = std::make_unique<SpuThread>(); bool ok = true;")
        out.append("\t  memset(vm::ptr(0x30000000), 0, 0x1000);")
        for k, wd in enumerate(words):
            out.append(f"\t  ppc_st32(t->ls + {4 * k}, 0x{wd:08X}u);")
        for k, v in init.items():
            if k == "mem":
                for a, ws in v.items():
                    for i, x in enumerate(ws):
                        out.append(f"\t  vm::write32(0x{a + 4 * i:X}, 0x{x:X}u);")
            else:
                for i, x in enumerate(v):
                    out.append(f"\t  VW(t->gpr[{k}], {i}) = 0x{x & 0xFFFFFFFF:X}u;")
        if 1 not in init:
            out.append("\t  VW(t->gpr[1], 0) = 0x3FFF0;")
        # ls initial data for the lqd test
        if name == "lqd / stqd":
            for i, x in enumerate([0xA, 0xB, 0xC, 0xD]):
                out.append(f"\t  ppc_st32(t->ls + 0x{0x200 + 4 * i:X}, 0x{x:X}u);")
        out.append("\t  uint32_t code = spu_run(*t); (void)code;")
        for k, v in exp.items():
            checks = []
            if k == "status":
                checks.append(("status", "t->exit_status", v))
            elif k == "mem":
                for a, ws in v.items():
                    for i, x in enumerate(ws):
                        checks.append((f"mem[{a + 4 * i:X}]", f"vm::read32(0x{a + 4 * i:X})", x))
            elif k == "ls":
                for a, ws in v.items():
                    for i, x in enumerate(ws):
                        checks.append((f"ls[{a + 4 * i:X}]", f"ppc_ld32(t->ls + 0x{a + 4 * i:X})", x))
            else:
                for i, x in enumerate(v):
                    checks.append((f"${k}[{i}]", f"VW(t->gpr[{k}], {i})", x & 0xFFFFFFFF))
            for label, got, want in checks:
                out.append(f'\t  if ((uint32_t)({got}) != 0x{want:X}u) {{ ok = false; printf("FAIL %-28s {label}: got 0x%X want 0x{want:X}\\n", '
                           f'"{name}", (unsigned)({got})); }}')
        out.append("\t  fails += !ok; }")
    out += [f'\tprintf("%d/%d SPU tests passed\\n", {len(TESTS)} - fails, {len(TESTS)});', "\treturn fails != 0;", "}"]
    src = os.path.join(root, "build", "spu_tests.cpp")
    exe = os.path.join(root, "build", "spu_tests.exe")
    os.makedirs(os.path.dirname(src), exist_ok=True)
    open(src, "w").write("\n".join(out) + "\n")
    rt = os.path.join(root, "runtime")
    srcs = [src, os.path.join(rt, "spu", "spu_interp.cpp"), os.path.join(rt, "spu", "spu_channels.cpp"),
            os.path.join(rt, "kernel", "memory.cpp"), os.path.join(rt, "kernel", "sync.cpp")]
    # minimal stand-ins for the parts of the runtime the SPU code links against
    stub = os.path.join(root, "build", "spu_test_stubs.cpp")
    open(stub, "w").write('#include "kernel/kernel.h"\n'
                          "uint64_t ppc_mftb() { return 0; }\n"
                          "GuestThread* current_thread() { return nullptr; }\n"
                          "uint32_t thread_id(GuestThread*) { return 0; }\n"
                          "int thread_join(uint32_t, uint64_t*) { return 0; }\n"
                          "int thread_detach(uint32_t) { return 0; }\n"
                          "void thread_exit(uint64_t) { throw 0; }\n"
                          "void syscall_register(uint32_t, const char*, SyscallFn) {}\n")
    subprocess.check_call(["clang++", "-std=c++20", "-O1", "-w", "-I", rt, *srcs, stub, "-o", exe, "-static",
                           "-Wl,--whole-archive", "-lpthread", "-Wl,--no-whole-archive"])
    return subprocess.call([exe])


if __name__ == "__main__":
    sys.exit(main())
