"""SPU static recompiler: embedded SPU images -> C++ (build/gen/spu_recomp.cpp).

Each image becomes one function that runs as a state machine:
  dispatch: switch (pc) { case <entry>: if (range invalid) break; goto L_<entry>; ... }
  L_<pc>:   exec_<op>(t, <word>, <pc>, npc)   // inlined from runtime/spu/spu_ops.h
Every taken branch goes back through dispatch; unknown targets and invalid ranges
return to the interpreter. See runtime/spu/spu_native.cpp for range validation.

Usage: python -I spu_recomp.py <EBOOT.elf> <out.cpp> <image-va> [<image-va> ...] [--overlays spu_overlays.txt]

--overlays adds raw code blobs the job kernel DMAs from the EBOOT into LS, as recorded by
`dante.exe --spu-profile` (data/spu_overlays.txt).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spu_isa import TABLE, sext  # noqa: E402

UNCOND_END = {"br", "bra", "bi", "iret", "stop", "stopd"}
CALLS = {"brsl", "brasl", "bisl", "bisled"}
REL_BRANCH = {"br", "brsl", "brz", "brnz", "brhz", "brhnz"}
ABS_BRANCH = {"bra", "brasl"}


def load_image(elf_path, va):
    d = open(elf_path, "rb").read()
    e = d[va - 0x10000:]
    entry = struct.unpack_from(">I", e, 24)[0]
    phoff = struct.unpack_from(">I", e, 28)[0]
    phnum = struct.unpack_from(">H", e, 44)[0]
    ls = bytearray(0x40000)
    exec_ranges, lo, hi = [], 0x40000, 0
    for i in range(phnum):
        t, o, v, _, fs, ms, fl, _ = struct.unpack_from(">8I", e, phoff + 32 * i)
        if t != 1 or not fs:
            continue
        ls[v:v + fs] = e[o:o + fs]
        lo, hi = min(lo, v), max(hi, v + fs)
        if fl & 1:
            exec_ranges.append((v, v + fs))
    return entry, bytes(ls), exec_ranges, lo, hi


def load_overlay(elf_path, ea, lsa, size):
    """Raw SPU code DMA'd straight from the EBOOT image (no ELF header) to a fixed LS address."""
    d = open(elf_path, "rb").read()
    ls = bytearray(0x40000)
    ls[lsa:lsa + size] = d[ea - 0x10000:ea - 0x10000 + size]
    return lsa, bytes(ls), [(lsa, lsa + size)], lsa, lsa + size


MIN_OVERLAY_LOADS = 50


def read_overlays(path, skip_ea_ranges):
    """spu_overlays.txt (from `dante.exe --spu-profile`): "ea lsa size count" per DMA. Contiguous
    chunks (the job kernel copies in 16 KB pieces) are merged; DMAs inside the ELF images
    already being translated are dropped."""
    rows = []
    for line in open(path):
        p = line.split()
        # rarely loaded copies are not worth the code size (and may be data sharing a code page)
        if len(p) >= 3 and (len(p) < 4 or int(p[3]) >= MIN_OVERLAY_LOADS):
            rows.append((int(p[0], 16), int(p[1], 16), int(p[2], 16)))
    rows.sort(key=lambda r: (r[1] - r[0], r[0]))  # chunks of one copy share lsa - ea
    merged = []
    for ea, lsa, size in rows:
        if merged:
            mea, mlsa, msize = merged[-1]
            if mlsa - mea == lsa - ea and mea <= ea <= mea + msize:
                merged[-1] = (mea, mlsa, max(msize, ea + size - mea))
                continue
        merged.append((ea, lsa, size))
    out = set()
    for ea, lsa, size in merged:
        if size < 0x100 or any(lo <= ea < hi for lo, hi in skip_ea_ranges):
            continue
        out.add((ea, lsa, min(size, 0x40000 - lsa) & ~3))
    return sorted(out)


def word(ls, a):
    return struct.unpack_from(">I", ls, a)[0]


def op_of(w):
    e = TABLE[w >> 21]
    return e[0] if e else None


def discover(ls, entry, exec_ranges, raw=False):
    def is_exec(a):
        return a % 4 == 0 and any(s <= a < e for s, e in exec_ranges)

    roots = {entry}
    # constants that look like code addresses (function pointers / job entry points)
    for s, e in exec_ranges:
        regs = {}
        for a in range(s, e, 4):
            w = word(ls, a)
            name = op_of(w)
            rt = w & 0x7F
            if name == "ila":
                v = (w >> 7) & 0x3FFFF
                if is_exec(v):
                    roots.add(v)
            elif name == "il":
                regs[rt] = sext((w >> 7) & 0xFFFF, 16) & 0xFFFFFFFF
                if is_exec(regs[rt]):
                    roots.add(regs[rt])
            elif name == "ilhu":
                regs[rt] = ((w >> 7) & 0xFFFF) << 16
            elif name == "iohl" and rt in regs:
                v = regs[rt] | ((w >> 7) & 0xFFFF)
                if is_exec(v):
                    roots.add(v)
    # code addresses stored as data words: function-pointer / jump tables (e.g. the job image's
    # dispatch table at LS 0x12000). A wrong guess only costs code size: entries are validated
    # against LS and only used when the SPU actually branches there.
    for s, e in exec_ranges:
        for a in range(s & ~3, e, 4):
            v = word(ls, a)
            if v and is_exec(v) and op_of(word(ls, v)) is not None:
                roots.add(v)
    if raw:
        # no symbol/entry information: functions usually start right after a return or jump
        for s, e in exec_ranges:
            for a in range(s, e - 4, 4):
                if op_of(word(ls, a)) in UNCOND_END and op_of(word(ls, a + 4)) is not None:
                    roots.add(a + 4)
    code, labels = set(), set()
    work = list(roots)
    labels |= roots
    while work:
        a = work.pop()
        while is_exec(a) and a not in code:
            w = word(ls, a)
            name = op_of(w)
            if name is None:
                break
            code.add(a)
            i16 = sext((w >> 7) & 0xFFFF, 16)
            target = None
            if name in REL_BRANCH:
                target = (a + (i16 << 2)) & 0x3FFFC
            elif name in ABS_BRANCH:
                target = (i16 << 2) & 0x3FFFC
            if target is not None and is_exec(target):
                labels.add(target)
                work.append(target)
            if name in CALLS:
                labels.add(a + 4)
            if name in UNCOND_END:
                break
            a += 4
    # every code word right after a non-code word also needs a label (range starts)
    return code, labels


def ranges_of(code):
    out, start, prev = [], None, None
    for a in sorted(code):
        if start is None:
            start = prev = a
        elif a == prev + 4:
            prev = a
        else:
            out.append((start, prev + 4))
            start = prev = a
    if start is not None:
        out.append((start, prev + 4))
    return out


def split_at_labels(rngs, labels):
    """Start a new range at every label: smaller ranges are cheaper to re-validate after DMA
    writes and a modified word (data mistaken for code) disables less native code."""
    out = []
    for s, e in rngs:
        start = s
        for a in range(s + 4, e, 4):
            if a in labels:
                out.append((start, a))
                start = a
        out.append((start, e))
    return out


EXITING_OPS = {"stop", "stopd", "rdch", "wrch", "heq", "heqi", "hgt", "hgti", "hlgt", "hlgti"}
STATE_OPS = {"stop"}  # spu_stop() may read/write t.gpr directly (SPU system calls)
# ops that may change npc (everything else falls through, so no exit path is emitted)
BRANCH_OPS = REL_BRANCH | ABS_BRANCH | {"bi", "bisl", "bisled", "biz", "binz", "bihz", "bihnz", "iret", "brsl", "brasl"}


def fmt_of(w):
    e = TABLE[w >> 21]
    return e[1] if e else None


def regs_of(w):
    """(referenced, written) SPU registers of one instruction, over-approximated from its fields:
    every field that may name a register is loaded; the destination field is the only write."""
    f0, f7, f14, f21 = w & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F, (w >> 21) & 0x7F
    if fmt_of(w) == "RRR":
        return {f0, f7, f14, f21}, f21
    return {f0, f7, f14}, f0


# Block-local register caching (copy the registers a block uses into a local array, write back
# at exits). Measured: no gain - a hot 308-instruction block runs at ~1 G instr/s either way
# (store-to-load forwarding already makes t.gpr traffic nearly free), and in one huge function
# per image it made the build take hours and ran slower. Kept switchable for experiments.
CACHE_REGS = False


def emit_block(idx, s, e, ls, labels, direct, out):
    """One straight-line block (every block starts at a label)."""
    words = [(a, word(ls, a)) for a in range(s, e, 4)]
    used = set()
    for _, w in words:
        used |= regs_of(w)[0]
    used = sorted(used) if CACHE_REGS else []
    out.append(f"L_{s:05X}:")
    out.append(f"    t.native_pc = 0x{s:05X}u;  // sampled by --spu-profile")
    out.append("    {")
    if CACHE_REGS:
        out.append("    PPCVR R[128];")
        out.append("    " + " ".join(f"R[{r}] = t.gpr[{r}];" for r in used))
    else:
        out.append("    PPCVR* const R = t.gpr;")
    dirty = set()

    def wb():
        return " ".join(f"t.gpr[{r}] = R[{r}];" for r in sorted(dirty)) if CACHE_REGS else ""

    for a, w in words:
        name_op = op_of(w)
        nxt = (a + 4) & 0x3FFFC
        dirty.add(regs_of(w)[1])
        i16 = sext((w >> 7) & 0xFFFF, 16)
        fast = ""
        if name_op in REL_BRANCH:
            fast = direct((a + (i16 << 2)) & 0x3FFFC)
        elif name_op in ABS_BRANCH:
            fast = direct((i16 << 2) & 0x3FFFC)
        branch = f" if (npc != 0x{nxt:05X}u) {{ {wb()} {fast}pc = npc; goto dispatch; }}" if name_op in BRANCH_OPS else ""
        if name_op in STATE_OPS:
            # flush, run on the real register file, then refresh the cached copies
            out.append(f"    {{ {wb()} uint32_t npc = 0x{nxt:05X}u; ++ni;"
                       f" if (uint64_t st = exec_{name_op}(t, t.gpr, 0x{w:08X}u, 0x{a:05X}u, npc)) {{ t.native_insn += ni; return st; }}"
                       + " " + " ".join(f"R[{r}] = t.gpr[{r}];" for r in used) +
                       f" if (npc != 0x{nxt:05X}u) {{ pc = npc; goto dispatch; }} }}")
            dirty.clear()
            continue
        call = f"exec_{name_op}(t, R, 0x{w:08X}u, 0x{a:05X}u, npc)"
        if name_op in EXITING_OPS:
            out.append(f"    {{ uint32_t npc = 0x{nxt:05X}u; ++ni;"
                       f" if (uint64_t st = {call}) {{ {wb()} t.native_insn += ni; return st; }}{branch} }}")
        else:
            out.append(f"    {{ uint32_t npc = 0x{nxt:05X}u; ++ni; {call};{branch} }}")
    out.append("    " + wb())
    out.append("    }")
    # falling off the end of the block into the next one
    end = e & 0x3FFFC
    nl = direct.next_block(end)
    if nl:
        out.append(f"    {nl}")
    out.append(f"    pc = 0x{end:05X}u;")
    out.append("    goto dispatch;")



def emit_image(idx, name, entry, ls, lo, hi, code, labels, out):
    rngs = split_at_labels(ranges_of(code), labels)
    rng_index = {}
    for ri, (s, e) in enumerate(rngs):
        for a in range(s, e, 4):
            rng_index[a] = ri
    labels = {a for a in labels if a in code} | {s for s, _ in rngs}
    fn = f"spu_img_{name}"
    out.append(f"// ---- image {name}: {len(code)} instructions, {len(rngs)} code ranges, {len(labels)} entries")
    out.append(f"static uint64_t {fn}(SpuThread& t, uint32_t pc, uint8_t* rs) {{")
    out.append("    uint32_t budget = 0;")
    out.append("    uint64_t ni = 0;  // instructions run here, flushed to t.native_insn on exit")
    out.append("dispatch:")
    # direct jumps below skip dispatch while (budget & 0x3FF) != 0x3FF, so this check still runs
    # at least every 1024 taken branches
    out.append("    if ((++budget & 0x3FF) == 0 && t.stop_req) { t.pc = pc; t.native_insn += ni; return SPU_EXIT(0); }")
    out.append("    switch (pc) {")
    for a in sorted(labels):
        out.append(f"        case 0x{a:05X}: if (rs[{rng_index[a]}] != 1 && !spu_validate_range(t, {idx}, {rng_index[a]})) break; goto L_{a:05X};")
    out.append("        default: break;")
    out.append("    }")
    out.append("    t.pc = pc;")
    out.append("    t.native_insn += ni;")
    out.append("    return 0;")
    class Direct:
        def __call__(self, target):
            """C++ that jumps straight to an in-image label once its range is known valid."""
            if target not in labels:
                return ""
            r = rng_index[target]
            return (f"if (npc == 0x{target:05X}u && (budget & 0x3FF) != 0x3FF && (rs[{r}] == 1 || spu_validate_range(t, {idx}, {r})))"
                    f" {{ ++budget; goto L_{target:05X}; }} ")

        def next_block(self, end):
            if end not in labels:
                return ""
            r = rng_index[end]
            return f"if (rs[{r}] == 1 || spu_validate_range(t, {idx}, {r})) goto L_{end:05X};"

    direct = Direct()

    for s, e in rngs:
        emit_block(idx, s, e, ls, labels, direct, out)
    out.append("}")
    out.append("")
    # data tables
    out.append(f"static const uint8_t {fn}_bytes[] = {{")
    blob = ls[lo:hi]
    for k in range(0, len(blob), 32):
        out.append("    " + ",".join(str(b) for b in blob[k:k + 32]) + ",")
    out.append("};")
    out.append(f"static const uint32_t {fn}_ranges[] = {{" + ",".join(f"0x{s:05X},0x{e:05X}" for s, e in rngs) + "};")
    rw = [-1] * ((hi - lo) // 4)
    for a, ri in rng_index.items():
        if lo <= a < hi:
            rw[(a - lo) // 4] = ri
    out.append(f"static const int16_t {fn}_rangeof[] = {{")
    for k in range(0, len(rw), 64):
        out.append("    " + ",".join(str(x) for x in rw[k:k + 64]) + ",")
    out.append("};")
    bm = [0] * 1024
    for a in labels:
        wi = a >> 2
        bm[wi >> 6] |= 1 << (wi & 63)
    out.append(f"static const uint64_t {fn}_entries[1024] = {{")
    for k in range(0, 1024, 8):
        out.append("    " + ",".join(f"0x{x:016X}ull" for x in bm[k:k + 8]) + ",")
    out.append("};")
    out.append("")
    return (f'    {{"{name}", 0x{lo:05X}, 0x{hi:05X}, {fn}_bytes, {fn}_ranges, {len(rngs)}, {fn}_rangeof, '
            f'{fn}_entries, {fn}}},'), len(code), len(rngs)


def main():
    args = sys.argv[1:]
    overlays_path = None
    if "--overlays" in args:
        i = args.index("--overlays")
        overlays_path = args[i + 1]
        del args[i:i + 2]
    elf, out_path, *vas = args
    out = ["// Generated by tools/spu_recomp.py - do not edit.", '#include "spu/spu_ops.h"', '#include "spu/spu_native.h"', ""]
    descs = []
    image_eas = []
    for va_s in vas:
        va = int(va_s, 16)
        entry, ls, exec_ranges, lo, hi = load_image(elf, va)
        image_eas.append((va, va + hi))
        code, labels = discover(ls, entry, exec_ranges)
        desc, n, nr = emit_image(len(descs), f"{va:08X}", entry, ls, lo, hi, code, labels, out)
        descs.append(desc)
        print(f"image 0x{va:08X}: LS 0x{lo:05X}-0x{hi:05X}, {n} instructions in {nr} ranges")
    if overlays_path and os.path.exists(overlays_path):
        for ea, lsa, size in read_overlays(overlays_path, image_eas):
            entry, ls, exec_ranges, lo, hi = load_overlay(elf, ea, lsa, size)
            code, labels = discover(ls, entry, exec_ranges, raw=True)
            if len(code) < 16:
                continue
            desc, n, nr = emit_image(len(descs), f"ov{ea:08X}_{lsa:05X}", entry, ls, lo, hi, code, labels, out)
            descs.append(desc)
            print(f"overlay EA 0x{ea:08X} -> LS 0x{lsa:05X}-0x{lsa + size:05X}: {n} instructions in {nr} ranges")
    out.append("const SpuImageDesc g_spu_images[] = {")
    out += descs
    out.append("};")
    out.append(f"const uint32_t g_spu_image_count = {len(descs)};")
    text = "\n".join(out) + "\n"
    if not (os.path.exists(out_path) and open(out_path).read() == text):
        open(out_path, "w").write(text)
        print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
