"""Symbolise build/host_prof.txt (written by `dante.exe --spu-profile`): host CPU time of the
SPU job threads by C++ function and by source line.

  python -I tools/host_prof.py [--lines N]
"""
import collections
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
EXE = os.path.join(ROOT, "build", "dante.exe")


def image_base():
    out = subprocess.run(["llvm-objdump", "-p", EXE], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.strip().startswith("ImageBase"):
            return int(line.split()[1], 16)
    return 0x140000000


def main():
    nlines = int(sys.argv[sys.argv.index("--lines") + 1]) if "--lines" in sys.argv else 40
    samples, dlls = [], []
    for line in open(os.path.join(ROOT, "build", "host_prof.txt")):
        if line.startswith("DLL "):
            _, name, n = line.split()
            dlls.append((name, int(n)))
            continue
        rva, n = line.split()
        samples.append((int(rva, 16), int(n)))
    base = image_base()
    addrs = "\n".join(f"0x{base + rva:X}" for rva, _ in samples)
    out = subprocess.run(["llvm-symbolizer", "--obj=" + EXE, "--functions=short", "--no-inlines", "--relative-address"],
                         input="\n".join(f"0x{rva:X}" for rva, _ in samples), capture_output=True, text=True).stdout
    blocks = out.strip().split("\n\n")
    by_fn, by_line = collections.Counter(), collections.Counter()
    total = sum(n for _, n in samples) + sum(n for _, n in dlls)
    for name, n in dlls:
        by_fn[name] += n
    for (rva, n), b in zip(samples, blocks):
        parts = b.splitlines()
        fn = parts[0] if parts else "?"
        loc = parts[1] if len(parts) > 1 else "?"
        by_fn[fn] += n
        path, line = (loc.rsplit(":", 2) + ["?", "?"])[:2] if loc.count(":") >= 2 else (loc, "?")
        by_line[fn + "  " + os.path.basename(path.replace("\\", "/")) + ":" + line] += n
    print(f"{total} samples")
    print("-- by function")
    for fn, n in by_fn.most_common(nlines):
        print(f"{100.0 * n / total:6.2f}%  {fn}")
    print("-- by line")
    for k, n in by_line.most_common(nlines):
        print(f"{100.0 * n / total:6.2f}%  {k}")


if __name__ == "__main__":
    main()
