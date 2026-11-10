#!/bin/sh
# Build and run the native smoke test for recompiled game functions.
# Usage (from dante-port/): sh tests/build_test.sh
set -e
ELF="../Dante's Inferno/PS3_GAME/USRDIR/EBOOT.elf"
python -I recomp/recomp.py "$ELF" analysis/eboot.json build/test_gen --only 0x0079E188,0x00324B70
clang++ -std=c++20 -O2 -w -Iruntime -Ibuild/test_gen \
  build/test_gen/ppc_recomp_000.cpp build/test_gen/ppc_func_table.cpp build/test_gen/ppc_imports_default.cpp \
  runtime/ppc_runtime.cpp runtime/kernel/*.cpp runtime/hle/*.cpp runtime/rsx/*.cpp runtime/spu/*.cpp tests/test_harness.cpp -o build/test_harness.exe -static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive
./build/test_harness.exe "$ELF"
clang++ -std=c++20 -O1 -w -Iruntime tests/atomic_test.cpp runtime/kernel/reservation.cpp -o build/atomic_test.exe \
  -static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive
./build/atomic_test.exe
