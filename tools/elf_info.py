"""Inspect a decrypted PS3 PPU ELF (EBOOT.elf).

Prints program headers, section headers, PRX imports (libraries + NIDs)
and scans for embedded SPU ELF images.

Usage: python -I elf_info.py <EBOOT.elf> [--nids nids.json] [--json out.json]
"""
import argparse
import json
import struct
import sys

PT_NAMES = {
    0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 6: "PHDR", 7: "TLS",
    0x60000001: "SCE_PARAM", 0x60000002: "SCE_PRX_PARAM", 0x700000A4: "SCE_PPURELA",
}


class Elf:
    def __init__(self, data):
        self.d = data
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
            raise ValueError("not a big-endian ELF64")
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, self.flags,
         _, self.phentsize, self.phnum, self.shentsize, self.shnum, self.shstrndx
         ) = struct.unpack_from(">HHIQQQIHHHHHH", data, 16)
        self.phdrs = []
        for i in range(self.phnum):
            p = struct.unpack_from(">IIQQQQQQ", data, self.phoff + i * self.phentsize)
            self.phdrs.append(dict(type=p[0], flags=p[1], offset=p[2], vaddr=p[3],
                                   paddr=p[4], filesz=p[5], memsz=p[6], align=p[7]))
        self.shdrs = []
        for i in range(self.shnum):
            s = struct.unpack_from(">IIQQQQIIQQ", data, self.shoff + i * self.shentsize)
            self.shdrs.append(dict(name_off=s[0], type=s[1], flags=s[2], addr=s[3],
                                   offset=s[4], size=s[5]))
        if self.shdrs:
            strtab = self.shdrs[self.shstrndx]
            for s in self.shdrs:
                s["name"] = self.cstr_at(strtab["offset"] + s["name_off"])

    def cstr_at(self, off):
        end = self.d.index(b"\0", off)
        return self.d[off:end].decode("latin-1")

    def v2o(self, va):
        for p in self.phdrs:
            if p["type"] == 1 and p["vaddr"] <= va < p["vaddr"] + p["filesz"]:
                return va - p["vaddr"] + p["offset"]
        return None

    def u32(self, va):
        o = self.v2o(va)
        return struct.unpack_from(">I", self.d, o)[0]

    def cstr(self, va):
        o = self.v2o(va)
        return self.cstr_at(o) if o is not None else None

    def prx_param(self):
        for p in self.phdrs:
            if p["type"] == 0x60000002:
                (size, magic, version, sdk, libent_start, libent_end, libstub_start,
                 libstub_end, ver, _) = struct.unpack_from(">IIIIIIIIHH", self.d, p["offset"])
                return dict(size=size, magic=magic, version=version, sdk=sdk,
                            libent_start=libent_start, libent_end=libent_end,
                            libstub_start=libstub_start, libstub_end=libstub_end)
        return None

    def imports(self):
        pp = self.prx_param()
        out = []
        va = pp["libstub_start"]
        while va < pp["libstub_end"]:
            o = self.v2o(va)
            size = self.d[o]
            (_, _, version, attr, nfunc, nvar, ntls, _, name_ptr, fnid_ptr, fstub_ptr,
             vnid_ptr, vstub_ptr, tnid_ptr, tstub_ptr) = struct.unpack_from(
                ">BBHHHHHIIIIIIII", self.d, o)
            lib = dict(name=self.cstr(name_ptr), version=version, attr=attr, funcs=[], vars=[])
            for i in range(nfunc):
                nid = self.u32(fnid_ptr + 4 * i)
                stub = self.u32(fstub_ptr + 4 * i)
                lib["funcs"].append(dict(nid=nid, stub=stub))
            for i in range(nvar):
                lib["vars"].append(dict(nid=self.u32(vnid_ptr + 4 * i)))
            out.append(lib)
            va += size
        return out


def scan_spu_elfs(data):
    found = []
    pos = data.find(b"\x7fELF", 1)
    while pos != -1:
        if pos + 0x34 <= len(data) and data[pos + 4] == 1 and data[pos + 5] == 2:
            machine = struct.unpack_from(">H", data, pos + 18)[0]
            if machine == 0x17:  # EM_SPU
                e_type, _, _, entry, phoff, shoff = struct.unpack_from(">HHIIII", data, pos + 16)
                phnum = struct.unpack_from(">H", data, pos + 44)[0]
                end = pos
                for i in range(phnum):
                    _, off, _, _, filesz, _, _, _ = struct.unpack_from(
                        ">IIIIIIII", data, pos + phoff + 32 * i)
                    end = max(end, pos + off + filesz)
                found.append(dict(offset=pos, entry=entry, phnum=phnum, approx_size=end - pos))
        pos = data.find(b"\x7fELF", pos + 1)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nids", help="JSON map of hex NID -> name")
    ap.add_argument("--json", help="write full results here")
    a = ap.parse_args()

    data = open(a.elf, "rb").read()
    e = Elf(data)
    names = {}
    if a.nids:
        names = {int(k, 16): v for k, v in json.load(open(a.nids)).items()}

    print(f"entry (OPD) 0x{e.entry:08x}  machine {e.machine}  type {e.type}")
    print("\nProgram headers:")
    for p in e.phdrs:
        print(f"  {PT_NAMES.get(p['type'], hex(p['type'])):14s} off=0x{p['offset']:08x} "
              f"va=0x{p['vaddr']:08x} filesz=0x{p['filesz']:08x} memsz=0x{p['memsz']:08x} "
              f"flags={p['flags']:#x}")
    print("\nSections:")
    for s in e.shdrs:
        if s["size"]:
            print(f"  {s.get('name',''):20s} addr=0x{s['addr']:08x} size=0x{s['size']:08x}")

    pp = e.prx_param()
    print(f"\nPRX param: sdk=0x{pp['sdk']:08x}")
    imps = e.imports()
    total = sum(len(l["funcs"]) for l in imps)
    resolved = 0
    print(f"\nImports: {len(imps)} libraries, {total} functions")
    for lib in imps:
        print(f"  {lib['name']:28s} {len(lib['funcs']):4d} funcs {len(lib['vars']):3d} vars")
        for f in lib["funcs"]:
            n = names.get(f["nid"])
            f["name"] = n
            if n:
                resolved += 1
    if names:
        print(f"\nNIDs resolved: {resolved}/{total}")

    spus = scan_spu_elfs(data)
    print(f"\nEmbedded SPU ELFs: {len(spus)}  (total ~{sum(s['approx_size'] for s in spus)} bytes)")

    if a.json:
        json.dump(dict(entry=e.entry, phdrs=e.phdrs,
                       sections=[{k: v for k, v in s.items()} for s in e.shdrs],
                       prx_param=pp, imports=imps, spu_elfs=spus), open(a.json, "w"), indent=1)
        print(f"wrote {a.json}")


if __name__ == "__main__":
    sys.exit(main())
