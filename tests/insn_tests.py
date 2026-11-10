"""Per-instruction semantic tests for the translator.

Each case: raw instruction word, initial state, expected state (hand-computed
from the PowerPC ISA, not from the translator). Generates a C++ test,
compiles it with clang++ and runs it.

Usage (from dante-port/): python -I tests/insn_tests.py
"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recomp"))
import ppc  # noqa: E402


def X(op, rd, ra, rb, xo, rc=0):
    return (op << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def M(op, rs, ra, sh, mb, me, rc=0):
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc


def MD(rs, ra, sh, mb, x3, rc=0):
    return ((30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((mb & 31) << 6)
            | ((mb >> 5) << 5) | (x3 << 2) | ((sh >> 5) << 1) | rc)


def D(op, rd, ra, imm):
    return (op << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)


# (name, word, init {reg: value}, expect {reg: value})
# regs: "r4", "ca", "f2" (double), "f1i" (f1 as raw u64), "cr7" (lt,gt,eq,so as 4-bit)
CASES = [
    ("rlwinm extract byte", M(21, 4, 3, 8, 24, 31), {"r4": 0x12345678}, {"r3": 0x12}),
    ("rlwinm wrap mask", M(21, 4, 3, 0, 28, 3), {"r4": 0xAAAAAAAA12345678}, {"r3": 0x1234567810000008}),
    ("rlwimi insert", M(20, 4, 3, 8, 16, 23), {"r3": 0xFFFFFFFF, "r4": 0x000000AB}, {"r3": 0xFFFFABFF}),
    ("srawi neg, bits lost", X(31, 4, 3, 4, 824), {"r4": 0xFFFFFFF1}, {"r3": 0xFFFFFFFFFFFFFFFF, "ca": 1}),
    ("srawi neg, exact", X(31, 4, 3, 4, 824), {"r4": 0xFFFFFFF0}, {"r3": 0xFFFFFFFFFFFFFFFF, "ca": 0}),
    ("srawi positive", X(31, 4, 3, 4, 824), {"r4": 0x7FFFFFF1}, {"r3": 0x07FFFFFF, "ca": 0}),
    ("subfic 0-0", D(8, 3, 4, 0), {"r4": 0}, {"r3": 0, "ca": 1}),
    ("subfic 0-1", D(8, 3, 4, 0), {"r4": 1}, {"r3": 0xFFFFFFFFFFFFFFFF, "ca": 0}),
    ("addic carry", D(12, 3, 4, 1), {"r4": 0xFFFFFFFFFFFFFFFF}, {"r3": 0, "ca": 1}),
    ("adde carry-in", X(31, 3, 4, 5, 138), {"r4": 0xFFFFFFFFFFFFFFFF, "r5": 0, "ca": 1}, {"r3": 0, "ca": 1}),
    ("subfe borrow", X(31, 3, 4, 5, 136), {"r4": 5, "r5": 3, "ca": 1}, {"r3": 0xFFFFFFFFFFFFFFFE, "ca": 0}),
    ("addze", X(31, 3, 4, 0, 202), {"r4": 7, "ca": 1}, {"r3": 8, "ca": 0}),
    ("sradi", (31 << 26) | (4 << 21) | (3 << 16) | (1 << 11) | (413 << 2), {"r4": 0x8000000000000001},
     {"r3": 0xC000000000000000, "ca": 1}),
    ("rldicl clrldi 32", MD(4, 3, 0, 32, 0), {"r4": 0xDEADBEEF12345678}, {"r3": 0x12345678}),
    ("rldicr sldi 8", MD(4, 3, 8, 55, 1), {"r4": 0x00FF00FF00FF00FF}, {"r3": 0xFF00FF00FF00FF00}),
    ("rldimi high word", MD(4, 3, 32, 0, 3), {"r3": 0x1111111122222222, "r4": 0xAAAAAAAABBBBBBBB},
     {"r3": 0xBBBBBBBB22222222}),
    ("cmpw signed", X(31, 7 << 2, 3, 4, 0), {"r3": 0xFFFFFFFF, "r4": 1}, {"cr7": 0b1000}),
    ("cmplw unsigned", X(31, 7 << 2, 3, 4, 32), {"r3": 0xFFFFFFFF, "r4": 1}, {"cr7": 0b0100}),
    ("cmpd ignores low-word sign", X(31, (7 << 2) | 1, 3, 4, 0), {"r3": 0xFFFFFFFF, "r4": 1}, {"cr7": 0b0100}),
    ("cmpwi equal", D(11, 7 << 2, 3, 0xFFFF), {"r3": 0xFFFFFFFF}, {"cr7": 0b0010}),
    ("mulhwu", X(31, 3, 4, 5, 11), {"r4": 0xFFFFFFFF, "r5": 0xFFFFFFFF}, {"r3": 0xFFFFFFFE}),
    ("mullw signed 64-bit product", X(31, 3, 4, 5, 235), {"r4": 0xFFFFFFFF, "r5": 2}, {"r3": 0xFFFFFFFFFFFFFFFE}),
    ("divwu by zero", X(31, 3, 4, 5, 459), {"r4": 10, "r5": 0}, {"r3": 0}),
    ("divw negative", X(31, 3, 4, 5, 491), {"r4": 0xFFFFFFF6, "r5": 3}, {"r3": 0xFFFFFFFD}),
    ("slw by 32", X(31, 4, 3, 5, 24), {"r4": 1, "r5": 32}, {"r3": 0}),
    ("srw by 4", X(31, 4, 3, 5, 536), {"r4": 0xFFFFFFFFFFFFFFF0, "r5": 4}, {"r3": 0x0FFFFFFF}),
    ("extsh", X(31, 4, 3, 0, 922), {"r4": 0x8000}, {"r3": 0xFFFFFFFFFFFF8000}),
    ("extsw", X(31, 4, 3, 0, 986), {"r4": 0x80000000}, {"r3": 0xFFFFFFFF80000000}),
    ("cntlzw zero", X(31, 4, 3, 0, 26), {"r4": 0}, {"r3": 32}),
    ("cntlzw ignores high word", X(31, 4, 3, 0, 26), {"r4": 0xFFFFFFFF00000001}, {"r3": 31}),
    ("andi. sets cr0", D(28, 4, 3, 0x00F0), {"r4": 0x0F}, {"r3": 0, "cr0": 0b0010}),
    ("addis negative", D(15, 3, 4, 0xFFFF), {"r4": 0x20000}, {"r3": 0x10000}),
    ("fctiwz truncates", X(63, 1, 0, 2, 15), {"f2": -3.7}, {"f1i_lo": 0xFFFFFFFD}),
    ("fctiwz saturates", X(63, 1, 0, 2, 15), {"f2": 1e20}, {"f1i_lo": 0x7FFFFFFF}),
    ("fmadds", (59 << 26) | (1 << 21) | (2 << 16) | (4 << 11) | (3 << 6) | (29 << 1),
     {"f2": 2.0, "f3": 3.0, "f4": 1.0}, {"f1": 7.0}),
    ("fsel", (63 << 26) | (1 << 21) | (2 << 16) | (4 << 11) | (3 << 6) | (23 << 1),
     {"f2": -0.5, "f3": 10.0, "f4": 20.0}, {"f1": 20.0}),
    ("fneg", X(63, 1, 0, 2, 40), {"f2": 1.5}, {"f1": -1.5}),
]


class DummyFn:
    def contains(self, t):
        return False

    def call(self, t):
        return f"/* call {t:X} */"

    def jump_table(self, a):
        return None


# ---- VMX cases. Vector values: ("w", [4 BE words]) / ("f", [4 floats]) /
# ("h", [8 halfwords]) / ("b", [16 bytes]); memory: "mem": {addr: bytes}.
def VA(d, a, b, c, xo):
    return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | xo


def VXF(d, a, b, xo):
    return (4 << 26) | (d << 21) | (a << 16) | (b << 11) | xo


NAN = float("nan")
SEQ = list(range(16))
VCASES = [
    ("vaddfp", VXF(1, 2, 3, 10), {"v2": ("f", [1, 2, 3, 4]), "v3": ("f", [.5] * 4)}, {"v1": ("f", [1.5, 2.5, 3.5, 4.5])}),
    ("vmaddfp a*c+b", VA(1, 2, 3, 4, 46), {"v2": ("f", [1, 2, 3, 4]), "v4": ("f", [2] * 4), "v3": ("f", [1] * 4)},
     {"v1": ("f", [3, 5, 7, 9])}),
    ("vnmsubfp b-a*c", VA(1, 2, 3, 4, 47), {"v2": ("f", [1, 2, 3, 4]), "v4": ("f", [2] * 4), "v3": ("f", [1] * 4)},
     {"v1": ("f", [-1, -3, -5, -7])}),
    ("vaddfp flushes denormal", VXF(1, 2, 3, 10), {"v2": ("f", [1e-40] * 4), "v3": ("f", [0] * 4)}, {"v1": ("w", [0] * 4)}),
    ("vrefp", VXF(1, 0, 3, 266), {"v3": ("f", [2, 4, .5, 1])}, {"v1": ("f", [.5, .25, 2, 1])}),
    ("vspltw element 2", VXF(1, 2, 3, 652), {"v3": ("w", [0x11, 0x22, 0x33, 0x44])}, {"v1": ("w", [0x33] * 4)}),
    ("vspltisw -1", VXF(1, 31, 0, 908), {}, {"v1": ("w", [0xFFFFFFFF] * 4)}),
    ("vspltish -16", VXF(1, 16, 0, 844), {}, {"v1": ("h", [0xFFF0] * 8)}),
    ("vperm", VA(1, 2, 3, 4, 43), {"v2": ("b", SEQ), "v3": ("b", [16 + x for x in SEQ]),
                                   "v4": ("b", [31, 0, 17, 2, 19, 4, 21, 6, 23, 8, 25, 10, 27, 12, 29, 14])},
     {"v1": ("b", [31, 0, 17, 2, 19, 4, 21, 6, 23, 8, 25, 10, 27, 12, 29, 14])}),
    ("vsldoi 4", VA(1, 2, 3, 4, 44), {"v2": ("w", [0xA0, 0xA1, 0xA2, 0xA3]), "v3": ("w", [0xB0, 0xB1, 0xB2, 0xB3])},
     {"v1": ("w", [0xA1, 0xA2, 0xA3, 0xB0])}),
    ("vmrghw", VXF(1, 2, 3, 140), {"v2": ("w", [0xA0, 0xA1, 0xA2, 0xA3]), "v3": ("w", [0xB0, 0xB1, 0xB2, 0xB3])},
     {"v1": ("w", [0xA0, 0xB0, 0xA1, 0xB1])}),
    ("vmrglw", VXF(1, 2, 3, 396), {"v2": ("w", [0xA0, 0xA1, 0xA2, 0xA3]), "v3": ("w", [0xB0, 0xB1, 0xB2, 0xB3])},
     {"v1": ("w", [0xA2, 0xB2, 0xA3, 0xB3])}),
    ("vmrghh", VXF(1, 2, 3, 76), {"v2": ("h", list(range(8))), "v3": ("h", list(range(8, 16)))},
     {"v1": ("h", [0, 8, 1, 9, 2, 10, 3, 11])}),
    ("vcmpgtfp. all true", VXF(1, 2, 3, 710 | 1024), {"v2": ("f", [2] * 4), "v3": ("f", [1] * 4)},
     {"v1": ("w", [0xFFFFFFFF] * 4), "cr6": 0b1000}),
    ("vcmpgtfp. none true", VXF(1, 2, 3, 710 | 1024), {"v2": ("f", [0] * 4), "v3": ("f", [1] * 4)},
     {"v1": ("w", [0] * 4), "cr6": 0b0010}),
    ("vcmpequw mixed", VXF(1, 2, 3, 134), {"v2": ("w", [1, 2, 3, 4]), "v3": ("w", [1, 0, 3, 0])},
     {"v1": ("w", [0xFFFFFFFF, 0, 0xFFFFFFFF, 0])}),
    ("vctsxs saturates", VXF(1, 0, 3, 970), {"v3": ("f", [1.9, -1.9, 3e9, NAN])},
     {"v1": ("w", [1, 0xFFFFFFFF, 0x7FFFFFFF, 0])}),
    ("vcfsx scale 1", VXF(1, 1, 3, 842), {"v3": ("w", [2, 0xFFFFFFFC, 0, 7])}, {"v1": ("f", [1, -2, 0, 3.5])}),
    ("vsro 4 octets", VXF(1, 2, 3, 1100), {"v2": ("b", SEQ), "v3": ("b", [0] * 15 + [0x20])},
     {"v1": ("b", [0, 0, 0, 0] + list(range(12)))}),
    ("vsumsws", VXF(1, 2, 3, 1928), {"v2": ("w", [1, 2, 3, 4]), "v3": ("w", [0, 0, 0, 10])}, {"v1": ("w", [0, 0, 0, 20])}),
    ("vslw", VXF(1, 2, 3, 388), {"v2": ("w", [1, 1, 1, 0x80000000]), "v3": ("w", [0, 4, 31, 1])},
     {"v1": ("w", [1, 0x10, 0x80000000, 0])}),
    ("vsraw", VXF(1, 2, 3, 900), {"v2": ("w", [0x80000000, 0x40, 0xFFFFFFFF, 8]), "v3": ("w", [4, 4, 31, 35])},
     {"v1": ("w", [0xF8000000, 4, 0xFFFFFFFF, 1])}),
    ("vpkuwum", VXF(1, 2, 3, 78), {"v2": ("w", [0x00010002, 0x00030004, 0x00050006, 0x00070008]),
                                   "v3": ("w", [0x0009000A, 0x000B000C, 0x000D000E, 0x000F0010])},
     {"v1": ("w", [0x00020004, 0x00060008, 0x000A000C, 0x000E0010])}),
    ("vadduhs saturates", VXF(1, 2, 3, 576), {"v2": ("h", [0xFFFF, 1, 2, 3, 4, 5, 6, 0x8000]), "v3": ("h", [1] * 7 + [0x8000])},
     {"v1": ("h", [0xFFFF, 2, 3, 4, 5, 6, 7, 0xFFFF])}),
    ("vsubuws saturates at 0", VXF(1, 2, 3, 1664), {"v2": ("w", [5, 0, 10, 1]), "v3": ("w", [3, 1, 10, 2])},
     {"v1": ("w", [2, 0, 0, 0])}),
    ("vsel", VA(1, 2, 3, 4, 42), {"v2": ("w", [0xFFFFFFFF] * 4), "v3": ("w", [0] * 4),
                                  "v4": ("w", [0xFFFF0000, 0, 0xFFFFFFFF, 0x0F0F0F0F])},
     {"v1": ("w", [0x0000FFFF, 0xFFFFFFFF, 0, 0xF0F0F0F0])}),
    ("vxor self = 0", VXF(1, 2, 2, 1220), {"v2": ("w", [1, 2, 3, 4])}, {"v1": ("w", [0] * 4)}),
    ("vandc", VXF(1, 2, 3, 1092), {"v2": ("w", [0xFF] * 4), "v3": ("w", [0x0F, 0, 0xFF, 0xF0])},
     {"v1": ("w", [0xF0, 0xFF, 0, 0x0F])}),
    ("vmaxfp", VXF(1, 2, 3, 1034), {"v2": ("f", [1, -1, 5, 0]), "v3": ("f", [2, -2, 4, 0])}, {"v1": ("f", [2, -1, 5, 0])}),
    # memory
    ("lvx ignores low 4 bits", X(31, 1, 3, 4, 103), {"r3": 0x100, "r4": 5, "mem": {0x100: SEQ}}, {"v1": ("b", SEQ)}),
    ("lvlx", X(31, 1, 3, 4, 519), {"r3": 0x100, "r4": 4, "mem": {0x100: SEQ}}, {"v1": ("b", list(range(4, 16)) + [0] * 4)}),
    ("lvrx", X(31, 1, 3, 4, 551), {"r3": 0x100, "r4": 4, "mem": {0x100: SEQ}}, {"v1": ("b", [0] * 12 + [0, 1, 2, 3])}),
    ("lvsl", X(31, 1, 3, 4, 6), {"r3": 0x100, "r4": 3}, {"v1": ("b", list(range(3, 19)))}),
    ("stvx big-endian", X(31, 1, 3, 4, 231), {"r3": 0x200, "r4": 0, "v1": ("w", [0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10])},
     {"mem": {0x200: list(range(1, 17))}}),
    ("stvewx one element", X(31, 1, 3, 4, 199), {"r3": 0x200, "r4": 8, "v1": ("w", [0x11111111, 0x22222222, 0x33333333, 0x44444444])},
     {"mem": {0x200: [0] * 8 + [0x33] * 4 + [0] * 4}}),
]
CASES += VCASES

ACC = {"w": ("VW", 4, "uint32_t"), "h": ("VH", 8, "uint16_t"), "b": ("VB", 16, "uint8_t")}


def fbits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def vec_elems(kind, vals):
    """-> (accessor, [(index, unsigned value)])"""
    if kind == "f":
        return "VW", [(i, fbits(float(x))) for i, x in enumerate(vals)]
    acc, n, _ = ACC[kind]
    assert len(vals) == n
    return acc, list(enumerate(vals))


def init_code(k, v):
    if k == "ca":
        return [f"ctx.xer.ca = {v};"]
    if k == "mem":
        return [f"base[0x{a + i:X}] = 0x{b:02X};" for a, bs in v.items() for i, b in enumerate(bs)]
    if k.startswith("v"):
        acc, el = vec_elems(*v)
        return [f"{acc}(ctx.v[{k[1:]}], {i}) = 0x{x:X}u;" for i, x in el]
    if k.startswith("r"):
        return [f"ctx.r[{k[1:]}] = 0x{v:X}ull;"]
    if k.startswith("f"):
        return [f"ctx.f[{k[1:]}].f64 = {float(v)!r};"]
    raise ValueError(k)


def check_code(k, v):
    """-> list of (label, got_expr, want_expr)"""
    if k == "ca":
        return [(k, "(uint64_t)ctx.xer.ca", f"{v}ull")]
    if k.startswith("cr"):
        return [(k, f"(uint64_t)ctx.cr[{k[2:]}].get()", f"{v}ull")]
    if k == "f1i_lo":
        return [(k, "(uint64_t)(uint32_t)ctx.f[1].u64", f"0x{v:X}ull")]
    if k == "mem":
        return [(f"mem[{a + i:X}]", f"(uint64_t)base[0x{a + i:X}]", f"0x{b:X}ull")
                for a, bs in v.items() for i, b in enumerate(bs)]
    if k.startswith("v"):
        acc, el = vec_elems(*v)
        return [(f"{k}[{i}]", f"(uint64_t){acc}(ctx.v[{k[1:]}], {i})", f"0x{x:X}ull") for i, x in el]
    if k.startswith("r"):
        return [(k, f"ctx.r[{k[1:]}]", f"0x{v:X}ull")]
    if k.startswith("f"):
        return [(k, f"ctx.f[{k[1:]}].u64", f"[]{{ double d = {float(v)!r}; uint64_t u; memcpy(&u, &d, 8); return u; }}()")]
    raise ValueError(k)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    out = ["#include <cstdio>", "#include <cstdlib>", '#include "ppc_context.h"', "static uint8_t* base;", ""]
    for n, (name, w, init, exp) in enumerate(CASES):
        mn, code = ppc.translate(w, 0x10000, DummyFn())
        assert code, f"{name}: translator does not support {mn} ({w:08X})"
        out.append(f"static void t{n}(PPCContext& ctx) {{ {code} }}")
    out += ["", "int main() {", "\tint fails = 0;", "\tbase = (uint8_t*)calloc(1, 0x10000);"]
    for n, (name, w, init, exp) in enumerate(CASES):
        out.append("\t{ static PPCContext ctx; memset(&ctx, 0, sizeof(ctx)); memset(base, 0, 0x10000); bool ok = true;")
        for k, v in init.items():
            out += [f"\t  {c}" for c in init_code(k, v)]
        out.append(f"\t  t{n}(ctx);")
        for k, v in exp.items():
            for label, got, want in check_code(k, v):
                out.append(f'\t  if ({got} != {want}) {{ ok = false; printf("FAIL %-30s {label}: got 0x%llX want 0x%llX\\n", "{name}", '
                           f"(unsigned long long)({got}), (unsigned long long)({want})); }}")
        out.append("\t  fails += !ok; }")
    out += [f'\tprintf("%d/%d instruction tests passed\\n", {len(CASES)} - fails, {len(CASES)});',
            "\treturn fails != 0;", "}"]
    os.makedirs(os.path.join(root, "build"), exist_ok=True)
    src = os.path.join(root, "build", "insn_tests.cpp")
    exe = os.path.join(root, "build", "insn_tests.exe")
    open(src, "w").write("\n".join(out) + "\n")
    subprocess.check_call(["clang++", "-std=c++20", "-O1", "-w", "-I", os.path.join(root, "runtime"), src,
                           os.path.join(root, "runtime", "kernel", "reservation.cpp"), "-o", exe, "-static",
                           "-Wl,--whole-archive", "-lpthread", "-Wl,--no-whole-archive"])
    return subprocess.call([exe])


if __name__ == "__main__":
    sys.exit(main())
