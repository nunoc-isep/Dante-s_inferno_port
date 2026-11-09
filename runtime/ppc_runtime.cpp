// Core services the recompiled code calls into: indirect calls, traps, HLE fallbacks.
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

uint64_t ppc_mftb() {
    // PS3 timebase runs at 79.8 MHz
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return (uint64_t)((unsigned __int128)ns * 79800000ull / 1000000000ull);
}
