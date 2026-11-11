#!/bin/sh
# Build and run the native smoke test for recompiled game functions.
# Usage (from dante-port/): sh tests/build_test.sh
set -e
ELF="../Dante's Inferno/PS3_GAME/USRDIR/EBOOT.elf"
python -I recomp/recomp.py "$ELF" analysis/eboot.json build/test_gen --only 0x0079E188,0x00324B70
clang++ -std=c++20 -march=x86-64-v3 -O2 -w -Iruntime -Ibuild/test_gen \
  build/test_gen/ppc_recomp_000.cpp build/test_gen/ppc_func_table.cpp build/test_gen/ppc_imports_default.cpp \
  runtime/ppc_runtime.cpp runtime/kernel/*.cpp runtime/hle/*.cpp runtime/rsx/*.cpp runtime/spu/*.cpp runtime/gpu/*.cpp runtime/input/*.cpp tests/spu_images_stub.cpp tests/test_harness.cpp -o build/test_harness.exe -static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive -ld3d11 -ld3dcompiler -ldxguid -luuid -lgdi32 -luser32 -lhid -lsetupapi -lwinmm -lsynchronization
./build/test_harness.exe "$ELF"
clang++ -std=c++20 -march=x86-64-v3 -O1 -w -Iruntime tests/atomic_test.cpp runtime/kernel/reservation.cpp -o build/atomic_test.exe \
  -static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive -lsynchronization
./build/atomic_test.exe
clang++ -std=c++20 -march=x86-64-v3 -O1 -w -Iruntime tests/gamepad_test.cpp runtime/input/gamepad.cpp runtime/kernel/memory.cpp \
  -o build/gamepad_test.exe -static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive -lhid -lsetupapi
./build/gamepad_test.exe
