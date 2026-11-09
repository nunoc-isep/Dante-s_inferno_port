"""Static recompiler driver: PS3 PPU ELF -> C++ source.

Usage:
  python -I recomp.py <EBOOT.elf> <eboot.json> <out_dir> [--only 0xADDR,...] [--per-file N]
  python -I recomp.py <EBOOT.elf> <eboot.json> --stats

eboot.json is produced by tools/elf_info.py (imports with names).
"""
import argparse
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from elf_info import Elf  # noqa: E402
import ppc  # noqa: E402

UNCOND_END = {"b", "blr", "bctr_tail", "trap"}


class Program:
    def __init__(self, elf_path, info_path):
        self.elf = Elf(open(elf_path, "rb").read())
        self.info = json.load(open(info_path))
        e = self.elf
        # code range: .init .. import stubs
        self.stub_lo, self.stub_hi = 0xCC14A4, 0xCC3AE4
        self.code_lo, self.code_hi = 0x10200, self.stub_lo
        self.imports = {}
        for lib in self.info["imports"]:
            for fdesc in lib["funcs"]:
                name = fdesc.get("name") or f"nid_{fdesc['nid']:08X}"
                self.imports[fdesc["stub"]] = (lib["name"], name, fdesc["nid"])
        self.words = {}
        o = e.v2o(self.code_lo)
        n = (self.code_hi - self.code_lo) // 4
        vals = struct.unpack_from(f">{n}I", e.d, o)
        self.word = lambda a: vals[(a - self.code_lo) >> 2]
        self.entry_desc = struct.unpack_from(">II", e.d, e.v2o(e.entry))
        self.discover()

    # ------------------------------------------------------------ discovery
    def in_code(self, a):
        return self.code_lo <= a < self.code_hi

    def discover(self):
        e = self.elf
        opd = next(s for s in e.shdrs if s["addr"] <= e.entry < s["addr"] + s["size"])
        starts = set()
        for k in range(opd["size"] // 8):
            fa, _toc = struct.unpack_from(">II", e.d, e.v2o(opd["addr"] + 8 * k))
            if self.in_code(fa):
                starts.add(fa)
        self.opd_count = len(starts)
        # direct call targets
        for a in range(self.code_lo, self.code_hi, 4):
            w = self.word(a)
            if w >> 26 == 18 and w & 1:
                t = (ppc.sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else a)) & 0xFFFFFFFF
                if self.in_code(t):
                    starts.add(t)
        starts.add(self.entry_desc[0])
        # branch targets that leave their function become entry points too
        for _ in range(8):
            ordered = sorted(starts)
            added = 0
            for idx, s in enumerate(ordered):
                end = ordered[idx + 1] if idx + 1 < len(ordered) else self.code_hi
                for a in range(s, end, 4):
                    w = self.word(a)
                    op = w >> 26
                    if op == 18 and not w & 1:
                        t = (ppc.sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else a)) & 0xFFFFFFFF
                    elif op == 16 and not w & 1:
                        t = (ppc.sext(w & 0xFFFC, 16) + (0 if w & 2 else a)) & 0xFFFFFFFF
                    else:
                        continue
                    if not (s <= t < end) and self.in_code(t) and t not in starts:
                        starts.add(t)
                        added += 1
            if not added:
                break
        ordered = sorted(starts)
        self.funcs = []
        for idx, s in enumerate(ordered):
            end = ordered[idx + 1] if idx + 1 < len(ordered) else self.code_hi
            real_end = end
            while real_end > s and self.word(real_end - 4) == 0:
                real_end -= 4
            self.funcs.append((s, real_end, end))
        self.starts = starts
        self.find_jump_tables()

    def find_jump_tables(self):
        """GCC switch tables: `lwzx; add rX,rX,rT; mtctr rX; bctr` followed by
        int32 offsets relative to the table start (which directly follows bctr).
        The entry count comes from the preceding `cmplwi crN, rI, MAX`."""
        self.jump_tables = {}
        self.data_ranges = {}  # addr -> end of embedded data
        self.jt_unresolved = 0
        for s, e, _ in self.funcs:
            for a in range(s, e, 4):
                if self.word(a) != 0x4E800420:  # bctr
                    continue
                if (self.word(a - 4) & 0xFC1FFFFF) != 0x7C0903A6:  # mtctr
                    continue
                table = a + 4
                count = None
                for b in range(a - 4, max(s, a - 64) - 4, -4):
                    w = self.word(b)
                    if w >> 26 == 10 and not (w >> 21) & 1:  # cmplwi (32-bit)
                        count = (w & 0xFFFF) + 1
                        break
                targets = []
                limit = count if count else 4096
                for k in range(limit):
                    ea = table + 4 * k
                    if ea >= e:
                        break
                    t = (table + ppc.sext(self.word(ea), 32)) & 0xFFFFFFFF
                    if not (s <= t < e) or t & 3 or (table <= t < table + 4 * (k + 1)):
                        break
                    targets.append(t)
                if not targets or (count and len(targets) != count):
                    self.jt_unresolved += 1
                    continue
                self.jump_tables[a] = targets
                self.data_ranges[table] = table + 4 * len(targets)

    # ------------------------------------------------------------ helpers for ppc.translate
    def make_fn(self, start, end):
        prog = self

        class Fn:
            def contains(self, t):
                return start <= t < end

            def call(self, t):
                if prog.stub_lo <= t < prog.stub_hi:
                    lib, name, nid = prog.imports.get(t, ("unknown", f"stub_{t:08X}", 0))
                    # the import stub saves the caller's TOC at 0x28(r1); the caller reloads it after
                    return f"PPC_IMPORT_CALL(ppc_import_{lib}_{name});"
                if t in prog.starts:
                    return f"sub_{t:08X}(ctx, base);"
                return f"PPC_CALL_INDIRECT(0x{t:X}ull);"

            def jump_table(self, addr):
                return prog.jump_tables.get(addr)

        return Fn()

    @staticmethod
    def is_terminator(last):
        """True if control can never fall off the end after this instruction."""
        if last is None:
            return False
        mn, code = last
        if mn in ("blr", "b", "data"):
            return True
        if code.startswith("PPC_TRAP"):
            return True
        return mn == "bcctr" and code.startswith("if (true)")

    def translate_func(self, start, end, next_start):
        fn = self.make_fn(start, end)
        body, labels, unsupported = [], set(), collections.Counter()
        last = None
        skip_until = 0
        for a in range(start, end, 4):
            if a < skip_until:
                continue
            if a in self.data_ranges:
                skip_until = self.data_ranges[a]
                body.append((a, "data", f"// jump table, {(skip_until - a) // 4} entries"))
                continue
            w = self.word(a)
            mn, code = ppc.translate(w, a, fn)
            if code is None:
                unsupported[mn] += 1
                code = f'PPC_UNIMPLEMENTED(0x{a:X}, "{mn}", 0x{w:08X});'
            for m in re.finditer(r"goto loc_([0-9A-F]{8});", code):
                labels.add(int(m.group(1), 16))
            body.append((a, mn, code))
            last = (mn, code)
        if body and body[-1][1] == "data":
            last = ("data", "")
        lines = [f"PPC_FUNC_IMPL(sub_{start:08X}) {{", f"\tPPC_FUNC_ENTER(0x{start:08X});"]
        for a, mn, code in body:
            if a in labels:
                lines.append(f"loc_{a:08X}:")
            lines.append(f"\t// {a:08X} {mn}")
            lines.append(f"\t{code}")
        if not self.is_terminator(last):
            if next_start in self.starts:
                lines.append(f"\tsub_{next_start:08X}(ctx, base); // fall-through")
            else:
                lines.append(f"\tPPC_FALLTHROUGH(0x{next_start:X});")
        lines.append("}")
        return "\n".join(lines), unsupported

    # ------------------------------------------------------------ outputs
    def stats(self):
        total = collections.Counter()
        insn_total = 0
        clean = 0
        for s, e, nxt in self.funcs:
            _, uns = self.translate_func(s, e, nxt)
            insn_total += (e - s) // 4
            total.update(uns)
            if not uns:
                clean += 1
        bad = sum(total.values())
        print(f"functions: {len(self.funcs)} (OPD {self.opd_count}), instructions: {insn_total}")
        print(f"unsupported instructions: {bad} ({100.0 * bad / insn_total:.2f}%)")
        print(f"functions fully translated: {clean} ({100.0 * clean / len(self.funcs):.1f}%)")
        print(f"jump tables: {len(self.jump_tables)} resolved, {self.jt_unresolved} unresolved, "
              f"{sum(e - s for s, e in self.data_ranges.items()) // 4} data words skipped")
        print("top unsupported:")
        for mn, c in total.most_common(40):
            print(f"  {mn:16s} {c}")
        return total

    def emit(self, out, only=None, per_file=2000, hle_dir=None):
        os.makedirs(out, exist_ok=True)
        written = [0]

        def write(name, text):
            """Write only when content changed, so incremental builds stay incremental."""
            path = os.path.join(out, name)
            if os.path.exists(path) and open(path).read() == text:
                return
            open(path, "w").write(text)
            written[0] += 1

        funcs = [f for f in self.funcs if not only or f[0] in only]
        hdr = ["#pragma once", '#include "ppc_context.h"', ""]
        for s in sorted(self.starts):
            hdr.append(f"PPC_FUNC(sub_{s:08X});")
        used_imports = sorted({v for v in self.imports.values()})
        hdr.append("")
        for lib, name, nid in used_imports:
            hdr.append(f"PPC_FUNC(ppc_import_{lib}_{name}); // NID 0x{nid:08X}")
        write("ppc_funcs.h", "\n".join(hdr) + "\n")

        files = 0
        for k in range(0, len(funcs), per_file):
            chunk = funcs[k:k + per_file]
            src = ['#include "ppc_funcs.h"', ""]
            for s, e, nxt in chunk:
                code, _ = self.translate_func(s, e, nxt)
                src.append(code)
                src.append("")
            write(f"ppc_recomp_{files:03d}.cpp", "\n".join(src))
            files += 1

        # function table: recompiled functions + import stubs (for indirect calls)
        entries = {f[0]: f"sub_{f[0]:08X}" for f in funcs}
        for stub, (lib, name, nid) in self.imports.items():
            entries[stub] = f"ppc_import_{lib}_{name}"
        table = ['#include "ppc_funcs.h"', "", "const PPCFuncMapping g_ppc_func_table[] = {"]
        for a in sorted(entries):
            table.append(f"\t{{ 0x{a:08X}, {entries[a]} }},")
        table += ["\t{ 0, nullptr }", "};", "",
                  f"const uint32_t g_ppc_import_stub_lo = 0x{self.stub_lo:08X};",
                  f"const uint32_t g_ppc_import_stub_hi = 0x{self.stub_hi:08X};", ""]
        write("ppc_func_table.cpp", "\n".join(table))

        # default stubs for every import not implemented by hand in runtime/hle
        implemented = set()
        if hle_dir and os.path.isdir(hle_dir):
            for root, _, names in os.walk(hle_dir):
                for fn_ in names:
                    if fn_.endswith(".cpp"):
                        implemented |= set(re.findall(r"^HLE_FUNC\((\w+),\s*(\w+)\)",
                                                      open(os.path.join(root, fn_)).read(), re.M))
        stubs = ['#include "ppc_funcs.h"', "", "// Auto-generated default HLE stubs: log + return 0.",
                 "// Implement a function with HLE_FUNC(lib, name) in runtime/hle/ to replace its stub.", ""]
        n_impl = 0
        for lib, name, nid in used_imports:
            if (lib, name) in implemented:
                n_impl += 1
                continue
            stubs.append(f'PPC_FUNC_IMPL(ppc_import_{lib}_{name}) {{ ppc_hle_unimplemented("{lib}", "{name}", 0x{nid:08X}u, ctx); }}')
        write("ppc_imports_default.cpp", "\n".join(stubs) + "\n")
        print(f"emitted {len(funcs)} functions into {files} files in {out} "
              f"({written[0]} files changed, {n_impl}/{len(used_imports)} imports implemented by HLE)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("info")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--only", help="comma-separated function addresses")
    ap.add_argument("--per-file", type=int, default=1000)
    ap.add_argument("--hle-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime"))
    a = ap.parse_args()
    p = Program(a.elf, a.info)
    if a.stats:
        p.stats()
    if a.out:
        only = {int(x, 16) for x in a.only.split(",")} if a.only else None
        p.emit(a.out, only, a.per_file, a.hle_dir)


if __name__ == "__main__":
    main()
