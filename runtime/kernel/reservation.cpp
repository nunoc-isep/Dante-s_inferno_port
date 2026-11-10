// 128-byte line reservations shared by the PPU (lwarx/stwcx.) and the SPU MFC
// (GETLLAR/PUTLLC/PUTLLUC, PUT). On Cell a store-conditional fails if *any*
// store hit the reserved 128-byte line, not only if the reserved word changed;
// lock-free PPU<->SPU queues depend on that. Each line has a version counter:
// writers bump it after writing, store-conditionals check it under one lock.
#include <atomic>
#include <mutex>

#include "../ppc_context.h"

static constexpr uint32_t kLineBits = 20;  // 1M counters (hashed); collisions only cause harmless retries
static std::atomic<uint32_t> g_versions[1u << kLineBits];
static std::mutex g_atomic_mutex;

static inline std::atomic<uint32_t>& line(uint32_t ea) { return g_versions[(ea >> 7) & ((1u << kLineBits) - 1)]; }

uint32_t ppc_line_version(uint32_t ea) { return line(ea).load(std::memory_order_acquire); }

void ppc_line_touch(uint32_t ea, uint32_t size) {
    if (!size) return;
    for (uint64_t a = ea & ~127u; a < (uint64_t)ea + size; a += 128) line((uint32_t)a).fetch_add(1, std::memory_order_release);
}

std::mutex& ppc_atomic_lock() { return g_atomic_mutex; }

bool ppc_store_conditional(PPCContext& ctx, uint8_t* base, uint32_t ea, uint64_t v, int size) {
    if (!ctx.reserve_valid || ctx.reserve_addr != ea) return false;
    ctx.reserve_valid = false;
    std::lock_guard<std::mutex> lk(g_atomic_mutex);
    if (line(ea).load(std::memory_order_acquire) != ctx.reserve_version) return false;
    if (size == 4) {
        if (ppc_ld32(base + ea) != (uint32_t)ctx.reserve_value) return false;
        ppc_st32(base + ea, (uint32_t)v);
    } else {
        if (ppc_ld64(base + ea) != ctx.reserve_value) return false;
        ppc_st64(base + ea, v);
    }
    line(ea).fetch_add(1, std::memory_order_release);
    return true;
}
