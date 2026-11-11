// Core services the recompiled code calls into: indirect calls, traps, HLE fallbacks.
#include <windows.h>
#include <intrin.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "kernel/kernel.h"

static size_t g_func_count = 0;

static const PPCFuncMapping* find_func(uint32_t guest) {
    if (!g_func_count)
        while (g_ppc_func_table[g_func_count].host) ++g_func_count;
    size_t lo = 0, hi = g_func_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t g = g_ppc_func_table[mid].guest;
        if (g == guest) return &g_ppc_func_table[mid];
        if (g < guest) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

[[noreturn]] static void fatal(PPCContext& ctx, const char* fmt, uint32_t a, uint32_t b = 0) {
    fprintf(stderr, "\n*** ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, "\n");
    dump_trace(ctx);
    fflush(stderr);
    _Exit(4);
}

void ppc_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    const PPCFuncMapping* m = find_func(target);
    if (!m) {
        if (PPCFunc cb = hle_callback_lookup(target)) return cb(ctx, base);
        fatal(ctx, "indirect call to unknown guest address 0x%08X (lr=0x%08X)", target, (uint32_t)ctx.lr);
    }
    if (target >= g_ppc_import_stub_lo && target < g_ppc_import_stub_hi)
        PPC_STORE_U64(ctx.r[1] + 0x28, ctx.r[2]);  // what the import stub would have done
    m->host(ctx, base);
}

void ppc_trap(PPCContext& ctx, uint32_t addr) { fatal(ctx, "guest trap instruction at 0x%08X", addr); }

void ppc_unimplemented(PPCContext& ctx, uint32_t addr, const char* mn, uint32_t word) {
    fprintf(stderr, "unimplemented instruction %s (0x%08X)\n", mn, word);
    fatal(ctx, "at 0x%08X", addr);
}

void ppc_unreachable(PPCContext& ctx, uint32_t addr) {
    fatal(ctx, "jump table at 0x%08X: unexpected target 0x%08X", addr, (uint32_t)ctx.ctr);
}

void ppc_hle_unimplemented(const char* lib, const char* name, uint32_t nid, PPCContext& ctx) {
    static std::mutex m;
    static std::map<std::string, int> seen;
    int n;
    {
        std::lock_guard<std::mutex> lk(m);
        n = seen[name]++;
    }
    if (n < 3)
        LOG_W("hle", "%s::%s (NID 0x%08X) not implemented, returning CELL_OK (r3=0x%llX r4=0x%llX lr=0x%08X)", lib,
              name, nid, (unsigned long long)ctx.r[3], (unsigned long long)ctx.r[4], (uint32_t)ctx.lr);
    ctx.r[3] = 0;
}

namespace {
// PS3 timebase runs at 79.8 MHz. It is read very often (PPU mftb, the SPU job kernel polls the
// decrementer millions of times a second), so use the invariant TSC - one rdtsc and a multiply -
// calibrated once against QueryPerformanceCounter.
struct Timebase {
    double scale;  // timebase ticks per TSC tick
    uint64_t tsc0;
    uint64_t tb0;
    Timebase() {
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&q1);
        while (q1.QuadPart - q0.QuadPart < f.QuadPart / 50);  // 20 ms
        uint64_t t1 = __rdtsc();
        double secs = (double)(q1.QuadPart - q0.QuadPart) / (double)f.QuadPart;
        scale = 79800000.0 * secs / (double)(t1 - t0);
        tsc0 = t1;
        tb0 = (uint64_t)((double)q1.QuadPart * 79800000.0 / (double)f.QuadPart);
    }
};
const Timebase& timebase() {
    static const Timebase tb;
    return tb;
}
const Timebase& g_tb_init = timebase();  // calibrate at startup, not on the first guest read
}  // namespace

// Accurate short sleeps. MinGW's std::this_thread::sleep_for returns at once for sub-millisecond
// times and std::condition_variable::wait_for rounds up to the ~15 ms system tick, so guest
// usleep() and SPU event polling use a high-resolution waitable timer (~0.5 ms granularity) for
// the bulk of the wait and yield the last stretch away.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
void host_sleep_us(uint64_t us) {
    if (!us) {
        SwitchToThread();
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    if (us >= 1500) {
        if (HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(us - 1000) * 10;  // the timer overshoots by up to ~1 ms
            if (SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        } else {
            Sleep((DWORD)(us / 1000 - 1));
        }
    }
    while (std::chrono::steady_clock::now() < deadline) SwitchToThread();
}

uint64_t ppc_mftb() {
    const Timebase& tb = timebase();
    return tb.tb0 + (uint64_t)((double)(__rdtsc() - tb.tsc0) * tb.scale);
}
