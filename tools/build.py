"""Incremental build of the PC executable (build/dante.exe).

  python -I tools/build.py            regenerate C++ (only changed files are rewritten), compile, link
  python -I tools/build.py --no-regen skip the recompiler step
  python -I tools/build.py -j 12      parallel jobs (default: CPU count)

Needs clang++ (tested with clang 16, x86_64-w64-windows-gnu).
"""
import argparse
import concurrent.futures
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GAME = os.path.abspath(os.path.join(ROOT, "..", "Dante's Inferno"))
GEN = os.path.join(ROOT, "build", "gen")
OBJ = os.path.join(ROOT, "build", "obj")

CXX = "clang++"
COMMON = ["-std=c++20", "-I", os.path.join(ROOT, "runtime"), "-I", GEN, "-w", "-fno-strict-aliasing"]
GEN_FLAGS = COMMON + ["-O1"]
RT_FLAGS = COMMON + ["-O2", "-g"]


def mtime(p):
    try:
        return os.path.getmtime(p)
    except OSError:
        return 0


def obj_for(src):
    rel = os.path.relpath(src, ROOT).replace(os.sep, "_").replace(".cpp", ".o")
    return os.path.join(OBJ, rel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-regen", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    os.makedirs(OBJ, exist_ok=True)

    if not a.no_regen:
        elf = os.path.join(GAME, "PS3_GAME", "USRDIR", "EBOOT.elf")
        subprocess.check_call([sys.executable, "-I", os.path.join(ROOT, "recomp", "recomp.py"), elf,
                               os.path.join(ROOT, "analysis", "eboot.json"), GEN])

    runtime_headers = glob.glob(os.path.join(ROOT, "runtime", "**", "*.h"), recursive=True)
    hdr_time = max(mtime(h) for h in runtime_headers)
    # generated code only sees the CPU headers, not the kernel/HLE ones
    gen_hdr_time = max(mtime(os.path.join(ROOT, "runtime", "ppc_context.h")),
                       mtime(os.path.join(ROOT, "runtime", "ppc_vmx.h")),
                       mtime(os.path.join(GEN, "ppc_funcs.h")))

    jobs = []
    for src in sorted(glob.glob(os.path.join(GEN, "*.cpp"))):
        jobs.append((src, GEN_FLAGS, gen_hdr_time))
    for src in sorted(glob.glob(os.path.join(ROOT, "runtime", "**", "*.cpp"), recursive=True)):
        jobs.append((src, RT_FLAGS, hdr_time))

    todo = [(s, f) for s, f, ht in jobs if mtime(obj_for(s)) < max(mtime(s), ht)]
    print(f"{len(todo)}/{len(jobs)} files to compile")
    t0 = time.time()
    failed = []

    def compile_one(job):
        src, flags = job
        r = subprocess.run([CXX, *flags, "-c", src, "-o", obj_for(src)], capture_output=True, text=True)
        return src, r.returncode, r.stderr

    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        for n, (src, rc, err) in enumerate(ex.map(compile_one, todo), 1):
            status = "ok" if rc == 0 else "FAILED"
            print(f"  [{n}/{len(todo)}] {os.path.relpath(src, ROOT)} {status}", flush=True)
            if rc:
                failed.append(src)
                print(err[:4000])
    if failed:
        print(f"{len(failed)} file(s) failed")
        return 1
    print(f"compiled in {time.time() - t0:.0f}s, linking...")
    exe = os.path.join(ROOT, "build", "dante.exe")
    objs = [obj_for(s) for s, _, _ in jobs]
    r = subprocess.run([CXX, "-o", exe, *objs, "-static", "-Wl,--whole-archive", "-lpthread", "-Wl,--no-whole-archive", "-Wl,--stack,16777216"], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:8000])
        return 1
    print(f"built {exe}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
