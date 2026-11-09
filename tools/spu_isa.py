"""Cell SPU instruction set: one table used for the Python disassembler and to
generate the C++ decoder (runtime/spu/spu_opcodes.inc).

Usage:
  python -I spu_isa.py gen <out.inc>
  python -I spu_isa.py dis <EBOOT.elf> <elf-va-of-spu-image>      disassemble an embedded SPU ELF
  python -I spu_isa.py stats <EBOOT.elf> <va> [<va> ...]           decode coverage
"""
import struct
import sys

# (name, format, opcode). The opcode is the value of the top N bits, N given by the format:
#   RRR: 4, RI18: 7, RI10: 8, RI16: 9, RI8: 10, RR / RI7: 11
OPS = [
    # RRR
    ("selb", "RRR", 0x8), ("shufb", "RRR", 0xB), ("mpya", "RRR", 0xC),
    ("fnms", "RRR", 0xD), ("fma", "RRR", 0xE), ("fms", "RRR", 0xF),
    # RI18
    ("hbra", "RI18", 0x08), ("hbrr", "RI18", 0x09), ("ila", "RI18", 0x21),
    # RI10
    ("ori", "RI10", 0x04), ("ohi", "RI10", 0x05), ("orbi", "RI10", 0x06),
    ("sfi", "RI10", 0x0C), ("sfhi", "RI10", 0x0D),
    ("andi", "RI10", 0x14), ("andhi", "RI10", 0x15), ("andbi", "RI10", 0x16),
    ("ai", "RI10", 0x1C), ("ahi", "RI10", 0x1D),
    ("stqd", "RI10", 0x24), ("lqd", "RI10", 0x34),
    ("xori", "RI10", 0x44), ("xorhi", "RI10", 0x45), ("xorbi", "RI10", 0x46),
    ("cgti", "RI10", 0x4C), ("cgthi", "RI10", 0x4D), ("cgtbi", "RI10", 0x4E), ("hgti", "RI10", 0x4F),
    ("clgti", "RI10", 0x5C), ("clgthi", "RI10", 0x5D), ("clgtbi", "RI10", 0x5E), ("hlgti", "RI10", 0x5F),
    ("mpyi", "RI10", 0x74), ("mpyui", "RI10", 0x75),
    ("ceqi", "RI10", 0x7C), ("ceqhi", "RI10", 0x7D), ("ceqbi", "RI10", 0x7E), ("heqi", "RI10", 0x7F),
    # RI16
    ("brz", "RI16", 0x040), ("stqa", "RI16", 0x041), ("brnz", "RI16", 0x042), ("brhz", "RI16", 0x044),
    ("brhnz", "RI16", 0x046), ("stqr", "RI16", 0x047), ("bra", "RI16", 0x060), ("lqa", "RI16", 0x061),
    ("brasl", "RI16", 0x062), ("br", "RI16", 0x064), ("fsmbi", "RI16", 0x065), ("brsl", "RI16", 0x066),
    ("lqr", "RI16", 0x067), ("il", "RI16", 0x081), ("ilhu", "RI16", 0x082), ("ilh", "RI16", 0x083),
    ("iohl", "RI16", 0x0C1),
    # RI8
    ("cflts", "RI8", 0x1D8), ("cfltu", "RI8", 0x1D9), ("csflt", "RI8", 0x1DA), ("cuflt", "RI8", 0x1DB),
    # RR / RI7
    ("stop", "RR", 0x000), ("lnop", "RR", 0x001), ("sync", "RR", 0x002), ("dsync", "RR", 0x003),
    ("mfspr", "RR", 0x00C), ("rdch", "RR", 0x00D), ("rchcnt", "RR", 0x00F),
    ("sf", "RR", 0x040), ("or", "RR", 0x041), ("bg", "RR", 0x042), ("sfh", "RR", 0x048), ("nor", "RR", 0x049),
    ("absdb", "RR", 0x053), ("rot", "RR", 0x058), ("rotm", "RR", 0x059), ("rotma", "RR", 0x05A),
    ("shl", "RR", 0x05B), ("roth", "RR", 0x05C), ("rothm", "RR", 0x05D), ("rotmah", "RR", 0x05E),
    ("shlh", "RR", 0x05F), ("roti", "RI7", 0x078), ("rotmi", "RI7", 0x079), ("rotmai", "RI7", 0x07A),
    ("shli", "RI7", 0x07B), ("rothi", "RI7", 0x07C), ("rothmi", "RI7", 0x07D), ("rotmahi", "RI7", 0x07E),
    ("shlhi", "RI7", 0x07F), ("a", "RR", 0x0C0), ("and", "RR", 0x0C1), ("cg", "RR", 0x0C2),
    ("ah", "RR", 0x0C8), ("nand", "RR", 0x0C9), ("avgb", "RR", 0x0D3),
    ("mtspr", "RR", 0x10C), ("wrch", "RR", 0x10D),
    ("biz", "RR", 0x128), ("binz", "RR", 0x129), ("bihz", "RR", 0x12A), ("bihnz", "RR", 0x12B),
    ("stopd", "RR", 0x140), ("stqx", "RR", 0x144),
    ("bi", "RR", 0x1A8), ("bisl", "RR", 0x1A9), ("iret", "RR", 0x1AA), ("bisled", "RR", 0x1AB),
    ("hbr", "RR", 0x1AC), ("gb", "RR", 0x1B0), ("gbh", "RR", 0x1B1), ("gbb", "RR", 0x1B2),
    ("fsm", "RR", 0x1B4), ("fsmh", "RR", 0x1B5), ("fsmb", "RR", 0x1B6),
    ("frest", "RR", 0x1B8), ("frsqest", "RR", 0x1B9), ("lqx", "RR", 0x1C4),
    ("rotqbybi", "RR", 0x1CC), ("rotqmbybi", "RR", 0x1CD), ("shlqbybi", "RR", 0x1CF),
    ("cbx", "RR", 0x1D4), ("chx", "RR", 0x1D5), ("cwx", "RR", 0x1D6), ("cdx", "RR", 0x1D7),
    ("rotqbi", "RR", 0x1D8), ("rotqmbi", "RR", 0x1D9), ("shlqbi", "RR", 0x1DB),
    ("rotqby", "RR", 0x1DC), ("rotqmby", "RR", 0x1DD), ("shlqby", "RR", 0x1DF),
    ("orx", "RR", 0x1F0), ("cbd", "RI7", 0x1F4), ("chd", "RI7", 0x1F5), ("cwd", "RI7", 0x1F6), ("cdd", "RI7", 0x1F7),
    ("rotqbii", "RI7", 0x1F8), ("rotqmbii", "RI7", 0x1F9), ("shlqbii", "RI7", 0x1FB),
    ("rotqbyi", "RI7", 0x1FC), ("rotqmbyi", "RI7", 0x1FD), ("shlqbyi", "RI7", 0x1FF),
    ("nop", "RR", 0x201), ("cgt", "RR", 0x240), ("xor", "RR", 0x241), ("cgth", "RR", 0x248), ("eqv", "RR", 0x249),
    ("cgtb", "RR", 0x250), ("sumb", "RR", 0x253), ("hgt", "RR", 0x258),
    ("clz", "RR", 0x2A5), ("xswd", "RR", 0x2A6), ("xshw", "RR", 0x2AE), ("cntb", "RR", 0x2B4), ("xsbh", "RR", 0x2B6),
    ("clgt", "RR", 0x2C0), ("andc", "RR", 0x2C1), ("fcgt", "RR", 0x2C2), ("dfcgt", "RR", 0x2C3),
    ("fa", "RR", 0x2C4), ("fs", "RR", 0x2C5), ("fm", "RR", 0x2C6), ("clgth", "RR", 0x2C8), ("orc", "RR", 0x2C9),
    ("fcmgt", "RR", 0x2CA), ("dfcmgt", "RR", 0x2CB), ("dfa", "RR", 0x2CC), ("dfs", "RR", 0x2CD), ("dfm", "RR", 0x2CE),
    ("clgtb", "RR", 0x2D0), ("hlgt", "RR", 0x2D8),
    ("addx", "RR", 0x340), ("sfx", "RR", 0x341), ("cgx", "RR", 0x342), ("bgx", "RR", 0x343),
    ("mpyhha", "RR", 0x346), ("mpyhhau", "RR", 0x34E),
    ("dfma", "RR", 0x35C), ("dfms", "RR", 0x35D), ("dfnms", "RR", 0x35E), ("dfnma", "RR", 0x35F),
    ("fscrrd", "RR", 0x398), ("fesd", "RR", 0x3B8), ("frds", "RR", 0x3B9), ("fscrwr", "RR", 0x3BA),
    ("dftsv", "RI7", 0x3BF),
    ("ceq", "RR", 0x3C0), ("fceq", "RR", 0x3C2), ("dfceq", "RR", 0x3C3), ("mpy", "RR", 0x3C4),
    ("mpyh", "RR", 0x3C5), ("mpyhh", "RR", 0x3C6), ("mpys", "RR", 0x3C7), ("ceqh", "RR", 0x3C8),
    ("fcmeq", "RR", 0x3CA), ("dfcmeq", "RR", 0x3CB), ("mpyu", "RR", 0x3CC), ("mpyhhu", "RR", 0x3CE),
    ("ceqb", "RR", 0x3D0), ("fi", "RR", 0x3D4), ("heq", "RR", 0x3D8),
]
BITS = {"RRR": 4, "RI18": 7, "RI10": 8, "RI16": 9, "RI8": 10, "RR": 11, "RI7": 11}


def build_table():
    """11-bit prefix -> (name, fmt). Shorter opcodes cover every prefix they match."""
    table = [None] * 2048
    for name, fmt, opc in OPS:
        shift = 11 - BITS[fmt]
        for low in range(1 << shift):
            idx = (opc << shift) | low
            if table[idx] is not None:
                raise ValueError(f"opcode clash: {name} vs {table[idx][0]} at {idx:03X}")
            table[idx] = (name, fmt)
    return table


TABLE = build_table()


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def disasm(w, pc):
    e = TABLE[w >> 21]
    if e is None:
        return f".long 0x{w:08X}"
    name, fmt = e
    rt, ra, rb, rc = w & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F, (w >> 21) & 0x7F
    if fmt == "RRR":
        return f"{name} ${(w >> 21) & 0x7F}, ${ra}, ${rb}, ${rt}"
    if fmt == "RI18":
        return f"{name} ${rt}, 0x{(w >> 7) & 0x3FFFF:X}"
    if fmt == "RI10":
        return f"{name} ${rt}, ${ra}, {sext((w >> 14) & 0x3FF, 10)}"
    if fmt == "RI16":
        i16 = sext((w >> 7) & 0xFFFF, 16)
        if name.startswith("br") or name in ("lqr", "stqr"):
            return f"{name} ${rt}, 0x{(pc + i16 * 4) & 0x3FFFF:05X}"
        return f"{name} ${rt}, 0x{(w >> 7) & 0xFFFF:X}"
    if fmt == "RI8":
        return f"{name} ${rt}, ${ra}, {(w >> 14) & 0xFF}"
    if fmt == "RI7":
        return f"{name} ${rt}, ${ra}, {sext(rb, 7)}"
    if name in ("stop", "stopd"):
        return f"{name} 0x{w & 0x3FFF:X}"
    if name in ("rdch", "rchcnt"):
        return f"{name} ${rt}, ch{ra}"
    if name == "wrch":
        return f"{name} ch{ra}, ${rt}"
    return f"{name} ${rt}, ${ra}, ${rb}"


def generate(out):
    lines = ["// Generated by tools/spu_isa.py - do not edit.", "// SPU_OP(name, format, 11-bit-prefix-first, prefix-count)"]
    for name, fmt, opc in OPS:
        shift = 11 - BITS[fmt]
        lines.append(f"SPU_OP({name}, {fmt}, 0x{opc << shift:03X}, {1 << shift})")
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"wrote {len(OPS)} opcodes to {out}")


def load_spu_elf(elf_path, va):
    d = open(elf_path, "rb").read()
    off = va - 0x10000  # code segment: file offset = va - 0x10000
    e = d[off:]
    entry = struct.unpack_from(">I", e, 24)[0]
    phoff = struct.unpack_from(">I", e, 28)[0]
    phnum = struct.unpack_from(">H", e, 44)[0]
    segs = []
    for i in range(phnum):
        t, o, v, _, fs, ms, fl, _ = struct.unpack_from(">8I", e, phoff + 32 * i)
        if t == 1:
            segs.append((v, e[o:o + fs], fl))
    return entry, segs


def main():
    cmd = sys.argv[1]
    if cmd == "gen":
        generate(sys.argv[2])
    elif cmd == "dis":
        entry, segs = load_spu_elf(sys.argv[2], int(sys.argv[3], 16))
        print(f"entry 0x{entry:05X}")
        for v, data, fl in segs:
            if not fl & 1:
                continue
            print(f"--- segment 0x{v:05X} ({len(data)} bytes)")
            for k in range(0, len(data) - 3, 4):
                w = struct.unpack_from(">I", data, k)[0]
                print(f"{v + k:05X}: {w:08X}  {disasm(w, v + k)}")
    elif cmd == "stats":
        import collections
        for va in sys.argv[3:]:
            entry, segs = load_spu_elf(sys.argv[2], int(va, 16))
            c = collections.Counter()
            unk = total = 0
            for v, data, fl in segs:
                if not fl & 1:
                    continue
                for k in range(0, len(data) - 3, 4):
                    w = struct.unpack_from(">I", data, k)[0]
                    e = TABLE[w >> 21]
                    total += 1
                    if e is None:
                        unk += 1
                    else:
                        c[e[0]] += 1
            print(f"{va}: {total} words, {unk} undecodable ({100.0 * unk / max(total, 1):.1f}%), top: "
                  + ", ".join(f"{n} {k}" for n, k in c.most_common(12)))


if __name__ == "__main__":
    main()
