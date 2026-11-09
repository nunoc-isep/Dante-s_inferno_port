# Dante's Inferno – native PC port (work in progress)

Static-recompilation port of the PS3 release (BLUS30405, v1.00).
This repository contains **only our own tools and code**. Game files (EBOOT,
VIV archives, extracted assets) are never committed — users supply their own
disc dump.

## Layout
```
dante-port/
  tools/        Python analysis tools
    elf_info.py     PPU ELF: segments, imports, SPU ELF scan
    build_nid_db.py resolve import NIDs -> names (SHA1(name+suffix))
    viv.py          list/extract BIGH .VIV archives
  analysis/     generated output (gitignored – derived from game binary)
```
Game dump expected at `../Dante's Inferno/` (PS3_GAME/USRDIR/EBOOT.elf decrypted via RPCS3).

## Findings so far (2026-11-09)

### Executable (EBOOT.elf)
| Item | Value |
|---|---|
| SDK | 2.70 (0x00270001), GCC 4.1.1 toolchain |
| Code segment | 0x00010000–0x00E94860 (~14.5 MB, `.text` ≈ 12.7 MB) |
| Data segment | 0x00EA0000, 2.6 MB file / 5.1 MB in memory |
| Entry (OPD) | 0x00EDFD00 |
| Approx. functions | ~52,800 (count of `blr`) |
| TLS | yes (0x24 init / 0x80C total) |

**PRX imports: 25 libraries, 306 functions – all 306 resolved** (`analysis/nids.json`).
Largest: sceNp (41), cellSysutil (38), sceNpCommerce2 (26), sys_net (25),
cellGcmSys (21), sysPrxForUser (17), cellSpurs (16), sys_fs/sys_io/cellHttp (14).
Online/commerce/trophy libs (sceNp*, cellHttp, cellSsl, sys_net, cellNetCtl)
can be stubbed to "offline" – ~120 of the 306.

**Direct LV2 syscalls:** 473 `sc` sites, 81 distinct syscall numbers
(threads, lwmutex/mutex/cond/event queues, memory, timers, SPU thread groups,
fs 801–834).

**Graphics:** game writes RSX command buffers itself (`cellGcmSetDefaultCommandBuffer`,
`_cellGcmSetFlipCommand`), so the port needs an RSX method → Vulkan/D3D12 translator.

### SPU code
7 embedded SPU ELFs, ~560 KB total:
| File offset | Size | Notes |
|---|---|---|
| 0xD67680 | 94 KB | game SPU module |
| 0xD80780 | 145 KB | game SPU module |
| 0xDA5C80 | 125 KB | game SPU module |
| 0xDC6500 | 125 KB | game SPU module |
| 0xE5F200 | 3.5 KB | libsputhread job helper (job_instance/swapper) |
| 0xE60300 | 6.6 KB | libdma/libc helper |
| 0xE62000 | 74 KB | game SPU module |
Work is dispatched through SPURS (16 cellSpurs imports). Additional SPU jobs
may live in VIV data – not yet checked.

### Assets (.VIV, format "BIGH")
Header: `BIGH`, u32 LE size, u32 BE count, u32 BE header length; then 12-byte
entries `{u32 BE offset, u32 BE size, u32 BE nameHash}` sorted by hash;
8-byte trailer `L283…`. Names are hashed only.
| Archive | Entries | Content |
|---|---|---|
| BIGFILE0 (2.7 GB) | 4,898 | mostly `0x03xx00xx` headers (EA audio, 48 kHz) + some `ols3` + VP6 |
| BIGFILE1 (2.4 GB) | 947 | `ols3` – EA SHOC-chunked level/asset data (`maptile_*`…) |
| BIGFILE2 (2.8 GB) | 43 | `MVhd` – VP6 FMV movies |

On PC the original asset files can be read straight from the user's VIVs at
runtime; no conversion is strictly required.

## Port architecture (planned)
1. **PPU recompiler** – translate the PowerPC64 code to C++ (based on the
   XenonRecomp approach, adapted for PS3 ELF input, OPD function descriptors
   and the TOC/r2 ABI). Cell PPU and Xenon share almost the same instruction
   set (standard VMX instead of VMX128).
2. **Runtime** – big-endian guest memory, HLE of the 306 imports and 81
   syscalls, threads/sync on Win32, cellFs → VIV/host file system.
3. **SPU** – start with an SPU interpreter running the 7 modules plus a
   minimal SPURS; recompile hot modules later.
4. **RSX** – parse command buffers, translate state + Cg-compiled shaders to
   Vulkan (or D3D12).
5. **Audio/video** – cellAudio → WASAPI/SDL; EA audio + VP6 decoders.

RPCS3's open-source code is the behavioral reference for every HLE module.

## PPU recompiler (prototype)
```
recomp/ppc.py        instruction -> C++ translator
recomp/recomp.py     function discovery + C++ emitter
runtime/ppc_context.h   guest CPU state, memory/byte-swap macros
runtime/ppc_runtime.cpp indirect calls, syscall/HLE fallbacks, timebase
tests/               native smoke test (sh tests/build_test.sh)
```
Generate everything (about 18 s, 241 MB of C++ in 54 files):
```
python -I recomp/recomp.py "../Dante's Inferno/PS3_GAME/USRDIR/EBOOT.elf" analysis/eboot.json build/gen --per-file 1000
python -I recomp/recomp.py "../Dante's Inferno/PS3_GAME/USRDIR/EBOOT.elf" analysis/eboot.json --stats
```
How it works:
- Function starts = OPD descriptors (50,749) ∪ `bl` targets ∪ branch targets
  that leave their function → **53,262 functions**, 3.33 M instructions.
- One C++ function per guest function, `goto` for local branches, direct C++
  calls for `bl`, tail calls for outgoing `b`, fall-through into the next
  function when code runs off the end (GCC save/restore helpers).
- Calls into the import stub area become `ppc_import_<lib>_<func>()`;
  default stubs are generated (log + return 0) until real HLE replaces them.
- `bctr` / `bctrl` go through `ppc_call_indirect` (address → function table).

- Switch jump tables (`lwzx; add; mtctr; bctr` + inline int32 offsets,
  bound from the preceding `cmplwi`) become C++ `switch` statements and the
  table words are skipped as data.

- VMX/AltiVec (`runtime/ppc_vmx.h`): vectors are stored byte-reversed so
  every element width stays consistent; ~130 instructions including the Cell
  `lvlx`/`lvrx` unaligned loads, CR6-updating compares, saturating ops and
  non-Java-mode denormal flushing. Currently portable scalar C++ (SSE
  versions of the hot ops are a later optimisation).

Status (v3, 2026-11-09):
| Metric | Value |
|---|---|
| Instructions translated | **100%** (0 unsupported of 3,327,144) |
| Functions fully translated | **100%** (53,262) |
| Jump tables | 487 resolved, 0 unresolved (11,876 data words skipped) |
| Generated code compiles (clang 16, -O1) | all 56 files, 0 errors |
| Instruction unit tests | 72/72 (37 scalar + 35 VMX, `python -I tests/insn_tests.py`) |
| Native execution test | 2 game `strlen` routines run correctly (8/8, `sh tests/build_test.sh`) |

The game uses 49 distinct VMX instructions; by volume `lvx` 39k, `stvx` 24k,
`vmaddfp` 23k, `vspltw` 13k, `vaddfp` 9k, `vsldoi` 9k, `vperm` 6k.

Known simplifications: XER[OV] is not tracked (the `o` forms compute the
right result only), vector estimates (`vrefp`, `vrsqrtefp`) are exact, and
loops split across "functions" by outgoing branches recurse on the C++ stack.

## Boot runtime

Build and run (needs clang++ 16+ and Python 3):
```
python -I tools/build.py          # regenerate (only changed files rewritten), compile, link -> build/dante.exe
build/dante.exe                   # finds the game dump by itself; options: --game <dump root> --trace --dump-after <s>
```
The first build compiles all 54 generated files (~10 min with 8 jobs); after
that, runtime/HLE changes rebuild in seconds.

```
runtime/main.cpp            boot: memory, ELF load, argv, main thread registers, run
runtime/kernel/memory.cpp   4 GB guest reservation, areas (main/user/kernel/mmapper/video/stack), allocator
runtime/kernel/process.cpp  ELF loader, PPU threads (1 host thread each), TLS, syscall dispatch,
                            HLE callbacks, crash handler with guest call trace
runtime/kernel/sync.cpp     lv2 mutex/cond/semaphore/rwlock/event queue+port, timers, time, memory, TTY
runtime/kernel/fs.cpp       VFS (/dev_bdvd, /app_home, /dev_hdd0...) + cellFs and sys_fs syscalls
runtime/hle/                sysPrxForUser, cellGcmSys, cellSysutil/cellSysmodule
runtime/rsx/                RSX command processor: jumps/calls, semaphores, flips (no drawing yet)
```
Any import implemented with `HLE_FUNC(lib, name)` anywhere under `runtime/`
automatically replaces the generated "not implemented" stub.

Boot progress (2026-11-09): the game runs its own startup code natively -
allocates 193 MB, loads system modules, initialises GCM (720p, two 1280x720
display buffers), starts 11 threads (workers, Job Manager, Presentation,
TextureWorker), opens BIGFILE0-2.VIV through the VFS and **submits its first
frame flip** (497 RSX methods). It then waits on the EA Job Manager, which runs
its jobs on SPU threads (syscalls 170/172/173/191, SPU image at 0xE6F200) that
are not emulated yet.

Lessons: MinGW's emulated `thread_local` is unreliable here - use the Win32
TLS slot (`current_thread()`); HLE import calls must store r2 at 0x28(r1)
like the real import stub, because callers reload the TOC from there.

## SPU emulation

```
tools/spu_isa.py            SPU instruction table (199 opcodes) -> disassembler + generated C++ decoder
runtime/spu/spu_interp.cpp  interpreter (128 x 128-bit registers, 256 KB local store)
runtime/spu/spu_channels.cpp channels: MFC DMA (single + list), GETLLAR/PUTLLC/PUTLLUC reservations,
                            mailboxes, signal notification, events (incl. lost-reservation), decrementer,
                            SPU -> PPU events (sys_spu_thread_send_event / throw_event), stop codes
runtime/spu/spu_lv2.cpp     syscalls 169-192 (thread groups, initialize/start/join, LS access, mailbox,
                            SNR, event ports) and sys_spu_image_import
tests/spu_tests.py          35 SPU programs assembled in Python and checked against hand-computed results
```
Disassemble an embedded SPU image: `python -I tools/spu_isa.py dis <EBOOT.elf> 0xE6F200`

Status (2026-11-09): both EA "Job Manager" SPU threads (image 0xE6F200) boot,
DMA their parameter blocks in and idle on their job queues at roughly 165 M
SPU instructions/s each. The PPU main thread is now blocked on **SPURS**
(`0x536DA0`, the game's SPURS wrapper): `cellSpurs*` is still stubbed, so the
three SPURS workloads (policy modules = the four SPU images loaded at 0x3000)
never run. `--dump-after N` now also lists SPU threads.

## SPURS and audio

`runtime/hle/cellSpurs.cpp` is a compact SPURS: the PPU API (attributes,
initialize, add/ready/shutdown/remove workload, event queues, thread ids,
finalize) plus a C++ SPU kernel per SPURS SPU that selects a ready workload
(within its max contention), copies the policy module to LS 0xA00 and starts it
exactly as the real kernel does ($0 = exit 0x818, $1 = 0x3FFB0, $3 = context
0x100, $4 = argument, $5 = poll status). The kernel services the module calls
through the context (poll/select at 0x290, exit at 0x818) are trapped with
reserved stop codes 0x3F00+.

The game's SPURS instance is `ears_job_ps3` - EA's audio runtime (EARS): four
persistent workloads share a 2 KB policy module at 0xE8B000 that fetches audio
jobs into LS 0x4000 and runs them. Analyse it with
`python -I tools/spu_isa.py raw <dump.bin> A00`.

`runtime/hle/cellAudio.cpp` provides ports, read indices and the 48 kHz mixer
clock (one 256-sample block every 5.33 ms + notify events). Output is silent
for now.

Boot progress (2026-11-09): past SPURS and audio init - pads, game data, NP,
**three frame flips**, audio DAC thread running, MoviePlayer2 decode thread
started. Current stop: the PPU "Job Manager - Job Thread" allocates from a pool
whose capacity is 0 (`0x8859B0`, null slot dereference) - something earlier
that sizes that pool still gets a stubbed answer.

## Next steps
- [ ] Find what sizes the Job Manager pool at 0x8859B0 (current crash)
- [ ] Audio output to a host device (WASAPI) from the cellAudio ports
- [ ] RSX: real rendering (NV4097 3D methods -> Vulkan/D3D12)
- [ ] cellPad / input, cellAudio, save data, remaining HLE (237 imports still default stubs)
- [ ] SPU performance: static recompilation of the SPU images (same approach as the PPU)
