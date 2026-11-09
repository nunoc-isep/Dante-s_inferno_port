// Minimal runtime services for the recompiled code (prototype).
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "ppc_context.h"

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

void ppc_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    const PPCFuncMapping* m = find_func(target);
    if (!m) {
        fprintf(stderr, "[ppc] indirect call to unknown address 0x%08X (lr=0x%08llX)\n", target,
                (unsigned long long)ctx.lr);
        abort();
    }
    m->host(ctx, base);
}

void ppc_syscall(PPCContext& ctx, uint8_t*, uint32_t addr) {
    fprintf(stderr, "[lv2] unimplemented syscall %llu at 0x%08X\n", (unsigned long long)ctx.r[11], addr);
    ctx.r[3] = 0;
}

void ppc_trap(PPCContext&, uint32_t addr) {
    fprintf(stderr, "[ppc] trap at 0x%08X\n", addr);
    abort();
}

void ppc_unimplemented(PPCContext&, uint32_t addr, const char* mn, uint32_t word) {
    fprintf(stderr, "[ppc] unimplemented instruction %s (0x%08X) at 0x%08X\n", mn, word, addr);
    abort();
}

void ppc_unreachable(PPCContext& ctx, uint32_t addr) {
    fprintf(stderr, "[ppc] unhandled jump-table target 0x%08llX at 0x%08X\n",
            (unsigned long long)ctx.ctr, addr);
    abort();
}

void ppc_hle_unimplemented(const char* lib, const char* name, uint32_t nid, PPCContext& ctx) {
    fprintf(stderr, "[hle] %s::%s (NID 0x%08X) not implemented, returning 0\n", lib, name, nid);
    ctx.r[3] = 0;
}

uint64_t ppc_mftb() {
    // PS3 timebase runs at 79.8 MHz
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return (uint64_t)((unsigned __int128)ns * 79800000ull / 1000000000ull);
}
