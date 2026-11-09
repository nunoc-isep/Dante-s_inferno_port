"""Reader for Dante's Inferno (PS3) BIGH .VIV archives.

Layout (verified against BIGFILE0-2 of BLUS30405):
  0x00  char[4]  "BIGH"
  0x04  u32 LE   archive size (== file size)
  0x08  u32 BE   entry count
  0x0C  u32 BE   header length (data starts after, 0x800-aligned)
  0x10  entry[count], 12 bytes each, sorted by hash:
          u32 BE offset (0x800-aligned), u32 BE size, u32 BE name hash
  then  8-byte trailer ("L283" + u32)
Filenames are not stored; entries are identified by hash.

Usage:
  python -I viv.py list    <file.viv> [--magic]
  python -I viv.py extract <file.viv> <out_dir> [hash ...]
"""
import os
import struct
import sys


def read_index(path):
    with open(path, "rb") as f:
        h = f.read(16)
        if h[:4] != b"BIGH":
            raise ValueError(f"{path}: not a BIGH archive")
        count, hlen = struct.unpack(">II", h[8:16])
        body = f.read(hlen - 16)
    return [dict(offset=o, size=s, hash=hs)
            for o, s, hs in (struct.unpack_from(">III", body, 12 * i) for i in range(count))]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd, path = sys.argv[1], sys.argv[2]
    ents = read_index(path)
    if cmd == "list":
        show_magic = "--magic" in sys.argv
        with open(path, "rb") as f:
            for e in ents:
                line = f"{e['hash']:08x}  off=0x{e['offset']:09x}  size={e['size']:10d}"
                if show_magic:
                    f.seek(e["offset"])
                    line += "  " + f.read(4).hex()
                print(line)
        print(f"{len(ents)} entries")
    elif cmd == "extract":
        out = sys.argv[3]
        want = {int(x, 16) for x in sys.argv[4:]}
        os.makedirs(out, exist_ok=True)
        n = 0
        with open(path, "rb") as f:
            for e in ents:
                if want and e["hash"] not in want:
                    continue
                f.seek(e["offset"])
                with open(os.path.join(out, f"{e['hash']:08x}.bin"), "wb") as o:
                    left = e["size"]
                    while left:
                        chunk = f.read(min(left, 1 << 20))
                        o.write(chunk)
                        left -= len(chunk)
                n += 1
        print(f"extracted {n} entries to {out}")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BrokenPipeError, OSError) as exc:  # output piped into head etc.
        if isinstance(exc, BrokenPipeError) or getattr(exc, "errno", None) == 22:
            sys.exit(0)
        raise
