#!/bin/sh
# Build and run the native smoke test for recompiled game functions.
# Usage (from dante-port/): sh tests/build_test.sh
set -e
ELF="../Dante's Inferno/PS3_GAME/USRDIR/EBOOT.elf"
python -I recomp/recomp.py "$ELF" analysis/eboot.json build/test_gen --only 0x0079E188,0x00324B70
clang++ -std=c++20 -O2 -Iruntime -Ibuild/test_gen -Wno-unused-label \
  build/test_gen/ppc_recomp_000.cpp build/test_gen/ppc_func_table.cpp \
  runtime/ppc_runtime.cpp tests/test_harness.cpp -o build/test_harness.exe
./build/test_harness.exe "$ELF"
