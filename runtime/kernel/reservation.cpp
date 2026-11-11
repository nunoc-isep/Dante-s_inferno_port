// 128-byte line reservations shared by the PPU (lwarx/stwcx.) and the SPU MFC
// (GETLLAR/PUTLLC/PUTLLUC, PUT). On Cell a store-conditional fails if *any*
// store hit the reserved 128-byte line, not only if the reserved word changed;
// lock-free PPU<->SPU queues depend on that. Each line has a version counter:
// writers bump it after writing, store-conditionals check it under one lock.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "../ppc_context.h"

static constexpr uint32_t kLineBits = 20;  // 1M counters (hashed); collisions only cause harmless retries
static std::atomic<uint32_t> g_versions[1u << kLineBits];
static std::mutex g_atomic_mutex;

static inline std::atomic<uint32_t>& line(uint32_t ea) { return g_versions[(ea >> 7) & ((1u << kLineBits) - 1)]; }

uint32_t ppc_line_version(uint32_t ea) { return line(ea).load(std::memory_order_acquire); }

// SPU threads blocked on a reservation-lost event register their line here; a store to that
// line (PPU stwcx., SPU PUT/PUTLLC) wakes them at once instead of after the polling timeout.
static constexpr int kWaitSlots = 16;
static std::atomic<uint32_t> g_wait_lines[kWaitSlots];  // line address + 1 (0 = free slot)
static std::atomic<uint32_t> g_wake_seq{0};

static inline void wake_line_waiters(uint32_t lo, uint32_t hi) {  // [lo, hi) byte range
    for (auto& w : g_wait_lines) {
        uint32_t v = w.load(std::memory_order_relaxed);
        if (v && v - 1 >= lo && v - 1 < hi) {
            g_wake_seq.fetch_add(1, std::memory_order_release);
            WakeByAddressAll(&g_wake_seq);
            return;
        }
    }
}

int ppc_rsv_wait_register(uint32_t line_addr) {
    for (int i = 0; i < kWaitSlots; ++i) {
        uint32_t expect = 0;
        if (g_wait_lines[i].compare_exchange_strong(expect, line_addr + 1)) return i;
    }
    return -1;  // all slots busy: the caller just polls
}
void ppc_rsv_wait_unregister(int slot) {
    if (slot >= 0) g_wait_lines[slot].store(0, std::memory_order_release);
}
uint32_t ppc_rsv_wake_seq() { return g_wake_seq.load(std::memory_order_acquire); }
// Block until a registered line is written or timeout_ms passes (whichever first).
void ppc_rsv_wait(uint32_t seen_seq, uint32_t timeout_ms) {
    WaitOnAddress(&g_wake_seq, &seen_seq, sizeof(seen_seq), timeout_ms);
}

void ppc_line_touch(uint32_t ea, uint32_t size) {
    if (!size) return;
    for (uint64_t a = ea & ~127u; a < (uint64_t)ea + size; a += 128) line((uint32_t)a).fetch_add(1, std::memory_order_release);
    wake_line_waiters(ea & ~127u, (uint32_t)std::min<uint64_t>((uint64_t)ea + size, 0xFFFFFFFFull));
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
    wake_line_waiters(ea & ~127u, (ea & ~127u) + 128);
    return true;
}
