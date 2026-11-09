"""Build a NID -> function name map by hashing candidate symbol names.

PS3 NIDs are the first 4 bytes (little-endian) of SHA1(name + SUFFIX).
Candidate names are harvested from any binaries passed in (e.g. rpcs3.exe,
which embeds the names of the HLE functions it implements), then filtered
down to the NIDs the game actually imports.

Usage: python -I build_nid_db.py <eboot_raw.json> <out.json> <binary> [binary...]
"""
import hashlib
import json
import re
import struct
import sys

SUFFIX = bytes([0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A])


def nid(name: bytes) -> int:
    return struct.unpack("<I", hashlib.sha1(name + SUFFIX).digest()[:4])[0]


def main():
    raw, out, *bins = sys.argv[1:]
    wanted = {f["nid"] for lib in json.load(open(raw))["imports"] for f in lib["funcs"]}
    found = {}
    ident = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{3,63}")
    for b in bins:
        data = open(b, "rb").read()
        for m in set(ident.findall(data)):
            n = nid(m)
            if n in wanted:
                found[f"{n:08x}"] = m.decode()
    json.dump(dict(sorted(found.items())), open(out, "w"), indent=1)
    print(f"resolved {len(found)}/{len(wanted)} imported NIDs -> {out}")


if __name__ == "__main__":
    main()
