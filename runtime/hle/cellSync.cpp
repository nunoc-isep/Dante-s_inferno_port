// cellSync mutex: CellSyncMutex = { u16 rel; u16 acq; } (big-endian, 4-byte aligned), shared with SPUs.
// Ticket lock: acquire takes ticket `acq++` and owns the lock while rel == ticket; unlock does rel++.
#include "../kernel/kernel.h"

namespace {
enum : uint32_t { CELL_SYNC_ERROR_BUSY = 0x8041010A, CELL_SYNC_ERROR_NULL_POINTER = 0x80410111,
                  CELL_SYNC_ERROR_ALIGN = 0x80410110 };

// atomically transform the 32-bit word (rel << 16 | acq) at guest address p
template <typename F>
bool update(uint32_t p, F f) {
    uint32_t* host = (uint32_t*)vm::ptr(p);
    for (;;) {
        uint32_t old_be = __atomic_load_n(host, __ATOMIC_ACQUIRE);
        uint32_t old = __builtin_bswap32(old_be), neu;
        if (!f(old, neu)) return false;
        uint32_t neu_be = __builtin_bswap32(neu);
        if (__atomic_compare_exchange_n(host, &old_be, neu_be, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
    }
}
}  // namespace

HLE_FUNC(cellSync, cellSyncMutexInitialize) {
    uint32_t p = ARG32(0);
    if (!p) return (void)RET(CELL_SYNC_ERROR_NULL_POINTER);
    if (p & 3) return (void)RET(CELL_SYNC_ERROR_ALIGN);
    vm::write32(p, 0);
    RET(CELL_OK);
}

HLE_FUNC(cellSync, cellSyncMutexTryLock) {
    uint32_t p = ARG32(0);
    if (!p) return (void)RET(CELL_SYNC_ERROR_NULL_POINTER);
    bool ok = update(p, [](uint32_t v, uint32_t& n) {
        uint16_t rel = v >> 16, acq = v & 0xFFFF;
        if (rel != acq) return false;  // someone holds it
        n = ((uint32_t)rel << 16) | (uint16_t)(acq + 1);
        return true;
    });
    RET(ok ? CELL_OK : CELL_SYNC_ERROR_BUSY);
}

HLE_FUNC(cellSync, cellSyncMutexUnlock) {
    uint32_t p = ARG32(0);
    if (!p) return (void)RET(CELL_SYNC_ERROR_NULL_POINTER);
    update(p, [](uint32_t v, uint32_t& n) {
        n = ((uint32_t)(uint16_t)((v >> 16) + 1) << 16) | (v & 0xFFFF);
        return true;
    });
    RET(CELL_OK);
}
