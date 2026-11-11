// SPU channel interface: MFC DMA (incl. list and atomic reservation commands),
// mailboxes, signal notification, events, decrementer, stop-and-signal.
#include <algorithm>
#include <chrono>
#include <cstring>

#include "../kernel/sync.h"
#include "spu.h"

// 128-byte line reservations shared with the PPU (runtime/kernel/reservation.cpp)
std::mutex& ppc_atomic_lock();
void ppc_line_touch(uint32_t ea, uint32_t size);

enum : uint32_t {
    MFC_PUTLLC_SUCCESS = 0, MFC_PUTLLC_FAILURE = 1, MFC_PUTLLUC_SUCCESS = 2, MFC_GETLLAR_SUCCESS = 4,
};
static constexpr uint64_t SYS_SPU_THREAD_EVENT_USER_KEY = 0xFFFFFFFF53505501ull;

SpuThread::SpuThread() {
    ls = (uint8_t*)_aligned_malloc(0x40000, 128);
    memset(ls, 0, 0x40000);
    memset(gpr, 0, sizeof(gpr));
}
SpuThread::~SpuThread() { _aligned_free(ls); }

static uint64_t timebase() { return ppc_mftb(); }
void host_sleep_us(uint64_t us);  // ppc_runtime.cpp: accurate short sleeps
// reservation.cpp: wake-ups for threads waiting on a reservation-lost event
int ppc_rsv_wait_register(uint32_t line_addr);
void ppc_rsv_wait_unregister(int slot);
uint32_t ppc_rsv_wake_seq();
void ppc_rsv_wait(uint32_t seen_seq, uint32_t timeout_ms);

// ---------------------------------------------------------------- DMA
static bool ea_ok(SpuThread& t, uint32_t ea, uint32_t size) {
    if ((uint64_t)ea + size <= 0xF0000000ull) return true;
    LOG_E("spu", "SPU 0x%X: DMA to unsupported address 0x%08X (size 0x%X) - LS/MMIO mapping not implemented", t.id,
          ea, size);
    return false;
}

static void dma(SpuThread& t, bool put, uint32_t lsa, uint32_t ea, uint32_t size) {
    if (!size || !ea_ok(t, ea, size)) return;
    lsa &= 0x3FFFF;
    if (lsa + size > 0x40000) size = 0x40000 - lsa;
    if (put) {
        vm::commit(ea, size);  // no-op when already committed; keeps a bad EA from crashing the host
        memcpy(vm::ptr(ea), t.ls + lsa, size);
        ppc_line_touch(ea, size);  // a PUT breaks PPU reservations on the lines it writes
    } else {
        memcpy(t.ls + lsa, vm::ptr(ea), size);
        spu_native_ls_written(t, lsa, size);
        for (uint32_t p = lsa >> 12; p <= ((lsa + size - 1) >> 12) && p < 64; ++p)
            t.ls_src[p] = ea + ((p << 12) - lsa);  // EA that maps to the start of this page
        if (g_spu_profile) {
            for (uint32_t p = lsa >> 12; p <= ((lsa + size - 1) >> 12) && p < 64; ++p) t.ls_ov[p] = nullptr;
            if (ea < 0x01000000) spu_note_overlay(t, ea, lsa, size);  // from the EBOOT image
        }
    }
}

static void dma_list(SpuThread& t, bool put, uint32_t lsa, uint32_t list_lsa, uint32_t list_size) {
    // list element (8 bytes, in LS): u16 stall/notify flag, u16 size (15 bits), u32 eal
    for (uint32_t off = 0; off < list_size; off += 8) {
        uint32_t ts = ppc_ld32(t.ls + ((list_lsa + off) & 0x3FFF8));
        uint32_t eal = ppc_ld32(t.ls + ((list_lsa + off + 4) & 0x3FFFC));
        uint32_t size = ts & 0x7FFF;
        dma(t, put, lsa, eal, size);
        lsa += (size + 15) & ~15u;
    }
}

static void mfc_command(SpuThread& t, uint32_t cmd) {
    uint32_t op = cmd & 0xFF;
    uint32_t lsa = t.mfc_lsa, ea = t.mfc_eal, size = t.mfc_size;
    switch (op) {
        case 0x20: case 0x21: case 0x22: case 0x28: case 0x29: case 0x2A:  // PUT, PUTB, PUTF, PUTS...
            dma(t, true, lsa, ea, size);
            break;
        case 0x40: case 0x41: case 0x42: case 0x48: case 0x49: case 0x4A:  // GET, GETB, GETF, GETS...
            dma(t, false, lsa, ea, size);
            break;
        case 0x24: case 0x25: case 0x26:  // PUTL
            dma_list(t, true, lsa, ea, size);
            break;
        case 0x44: case 0x45: case 0x46:  // GETL
            dma_list(t, false, lsa, ea, size);
            break;
        case 0xD0: {  // GETLLAR
            uint32_t line = ea & ~127u;
            std::lock_guard<std::mutex> lk(ppc_atomic_lock());
            t.rsv_version = ppc_line_version(line);
            memcpy(t.rsv_data, vm::ptr(line), 128);
            memcpy(t.ls + (lsa & 0x3FF80), t.rsv_data, 128);
            spu_native_ls_written(t, lsa & 0x3FF80, 128);
            t.rsv_addr = line;
            t.rsv_valid = true;
            t.atomic_stat = MFC_GETLLAR_SUCCESS;
            break;
        }
        case 0xB4: {  // PUTLLC
            uint32_t line = ea & ~127u;
            std::lock_guard<std::mutex> lk(ppc_atomic_lock());
            bool ok = t.rsv_valid && t.rsv_addr == line && ppc_line_version(line) == t.rsv_version &&
                      memcmp(vm::ptr(line), t.rsv_data, 128) == 0;
            if (ok) {
                memcpy(vm::ptr(line), t.ls + (lsa & 0x3FF80), 128);
                ppc_line_touch(line, 128);
            }
            t.rsv_valid = false;
            t.atomic_stat = ok ? MFC_PUTLLC_SUCCESS : MFC_PUTLLC_FAILURE;
            break;
        }
        case 0xB0: case 0xB8: {  // PUTLLUC, PUTQLLUC
            uint32_t line = ea & ~127u;
            std::lock_guard<std::mutex> lk(ppc_atomic_lock());
            memcpy(vm::ptr(line), t.ls + (lsa & 0x3FF80), 128);
            ppc_line_touch(line, 128);
            if (t.rsv_addr == line) t.rsv_valid = false;
            t.atomic_stat = MFC_PUTLLUC_SUCCESS;
            break;
        }
        case 0xA0: case 0xA1: case 0xA2:  // SNDSIG
            LOG_W("spu", "SPU 0x%X: SNDSIG to 0x%08X not implemented", t.id, ea);
            break;
        case 0xC0: case 0xC8: case 0xCC:  // barrier / eieio / sync
            break;
        default:
            LOG_E("spu", "SPU 0x%X: unknown MFC command 0x%X (lsa 0x%X ea 0x%X size 0x%X)", t.id, cmd, lsa, ea, size);
            break;
    }
    LOG_T("spu", "SPU 0x%X MFC cmd 0x%02X lsa 0x%05X ea 0x%08X size 0x%X tag %u", t.id, op, lsa, ea, size, t.mfc_tag);
}

// ---------------------------------------------------------------- events
static uint32_t pending_events(SpuThread& t) {
    uint32_t ev = t.event_stat;
    // lost when the line changed, or when anything (stwcx., another SPU's PUT) stored to it even
    // with identical data - plain PPU stores do not bump the version, hence the memcmp as well
    if (t.rsv_valid &&
        (ppc_line_version(t.rsv_addr) != t.rsv_version || memcmp(vm::ptr(t.rsv_addr), t.rsv_data, 128) != 0)) {
        t.rsv_valid = false;
        t.event_stat |= SPU_EVENT_LR;
        ev |= SPU_EVENT_LR;
    }
    if (!t.in_mbox.empty()) ev |= SPU_EVENT_MB;
    if (t.snr_valid[0]) ev |= SPU_EVENT_S1;
    if (t.snr_valid[1]) ev |= SPU_EVENT_S2;
    if (t.dec_armed && (int32_t)(t.dec_value - (uint32_t)(timebase() - t.dec_start)) < 0) {
        t.dec_armed = false;
        t.event_stat |= SPU_EVENT_TM;
        ev |= SPU_EVENT_TM;
    }
    return ev;
}

// ---------------------------------------------------------------- channels
bool spu_read_channel(SpuThread& t, uint32_t ch, uint32_t& value) {
    struct StateReset {
        SpuThread& t;
        ~StateReset() { t.state = "running"; }
    } reset{t};
    switch (ch) {
        case SPU_RdInMbox: {
            std::unique_lock<std::mutex> lk(t.m);
            while (t.in_mbox.empty()) {
                t.state = "waiting on in-mailbox (ch29)";
                if (t.stop_req) return false;
                t.cv.wait_for(lk, std::chrono::milliseconds(10));
            }
            value = t.in_mbox.front();
            t.in_mbox.pop_front();
            return true;
        }
        case SPU_RdSigNotify1: case SPU_RdSigNotify2: {
            int n = ch == SPU_RdSigNotify1 ? 0 : 1;
            std::unique_lock<std::mutex> lk(t.m);
            while (!t.snr_valid[n]) {
                t.state = n ? "waiting on signal 2" : "waiting on signal 1";
                if (t.stop_req) return false;
                t.cv.wait_for(lk, std::chrono::milliseconds(10));
            }
            value = t.snr[n];
            t.snr[n] = 0;
            t.snr_valid[n] = false;
            return true;
        }
        case SPU_RdEventStat: {
            // Reservation-lost and decrementer events have no direct notifier. Yield-poll for a
            // short while (low latency), then block: on the reservation line (stwcx./PUT to it wake
            // us at once; plain PPU stores are caught by the 1 ms timeout), or on a precise sleep
            // when the decrementer is about to expire.
            const auto t0 = std::chrono::steady_clock::now();
            int slot = -1;
            uint32_t slot_line = 0;
            struct Unreg {
                int& s;
                ~Unreg() { ppc_rsv_wait_unregister(s); }
            } unreg{slot};
            for (;;) {
                uint32_t seq = ppc_rsv_wake_seq();
                uint64_t dec_us = ~0ull;
                bool lr_wait;
                {
                    std::unique_lock<std::mutex> lk(t.m);
                    uint32_t ev = pending_events(t) & t.event_mask;
                    if (ev) {
                        value = ev;
                        return true;
                    }
                    if (!t.event_mask) {
                        LOG_W("spu", "SPU 0x%X reads event status with an empty mask (would block forever)", t.id);
                        value = 0;
                        return true;
                    }
                    t.state = "waiting on events (ch0)";
                    if (t.stop_req) return false;
                    if (t.dec_armed && (t.event_mask & SPU_EVENT_TM)) {
                        int32_t left = (int32_t)(t.dec_value - (uint32_t)(timebase() - t.dec_start));
                        dec_us = left > 0 ? (uint64_t)left / 80 : 0;  // 79.8 MHz
                    }
                    lr_wait = t.rsv_valid && (t.event_mask & SPU_EVENT_LR);
                    if (lr_wait && slot_line != t.rsv_addr) {
                        ppc_rsv_wait_unregister(slot);
                        slot = ppc_rsv_wait_register(t.rsv_addr);
                        slot_line = t.rsv_addr;
                    }
                }
                if (std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(50)) {
                    host_sleep_us(0);  // yield
                } else if (dec_us < 50) {
                    host_sleep_us(dec_us);  // about to expire
                } else {
                    // Block ~1 ms (or until the reservation line is written). The SPURS policy
                    // modules poll with sub-millisecond decrementer timeouts; waking a little late
                    // for a timer is harmless, spinning for it would burn a core per SPU.
                    ppc_rsv_wait(seq, 1);
                }
            }
        }
        case SPU_RdEventMask: value = t.event_mask; return true;
        case SPU_RdDec: value = t.dec_value - (uint32_t)(timebase() - t.dec_start); return true;
        case MFC_RdTagStat: value = t.tag_mask; return true;  // DMA is synchronous: everything is complete
        case MFC_RdTagMask: value = t.tag_mask; return true;
        case MFC_RdAtomicStat: value = t.atomic_stat; return true;
        case MFC_RdListStallStat: value = 0; return true;
        case SPU_RdSRR0: value = t.srr0; return true;
        case SPU_RdMachStat: value = 1; return true;  // problem state
        default:
            LOG_E("spu", "SPU 0x%X: read from unsupported channel %u at 0x%05X", t.id, ch, t.pc);
            value = 0;
            return true;
    }
}

bool spu_write_channel(SpuThread& t, uint32_t ch, uint32_t value) {
    switch (ch) {
        case MFC_LSA: t.mfc_lsa = value; return true;
        case MFC_EAH: t.mfc_eah = value; return true;
        case MFC_EAL: t.mfc_eal = value; return true;
        case MFC_Size: t.mfc_size = value & 0xFFFF; return true;
        case MFC_TagID: t.mfc_tag = value & 0x1F; return true;
        case MFC_Cmd: mfc_command(t, value); return true;
        case MFC_WrTagMask: t.tag_mask = value; return true;
        case MFC_WrTagUpdate: return true;
        case MFC_WrListStallAck: return true;
        case MFC_WrMSSyncReq: return true;
        case SPU_WrEventMask: {
            std::lock_guard<std::mutex> lk(t.m);
            t.event_mask = value;
            return true;
        }
        case SPU_WrEventAck: {
            std::lock_guard<std::mutex> lk(t.m);
            t.event_stat &= ~value;
            return true;
        }
        case SPU_WrDec:
            t.dec_value = value;
            t.dec_start = timebase();
            t.dec_armed = true;
            return true;
        case SPU_WrSRR0: t.srr0 = value; return true;
        case SPU_WrOutMbox: {
            std::lock_guard<std::mutex> lk(t.m);
            t.out_mbox = value;
            t.out_mbox_valid = true;
            return true;
        }
        case SPU_WrOutIntrMbox: {
            uint32_t code = value >> 24;
            if (code < 128) {  // sys_spu_thread_send_event (<64, waits for a reply) / throw_event
                uint32_t spup = code & 63, data;
                {
                    std::lock_guard<std::mutex> lk(t.m);
                    data = t.out_mbox;
                    t.out_mbox_valid = false;
                }
                auto q = t.spup[spup].lock();
                uint32_t res = CELL_OK;
                if (q) q->push(SYS_SPU_THREAD_EVENT_USER_KEY, t.id, ((uint64_t)spup << 32) | (value & 0x00FFFFFF), data);
                else res = 0x80010013;  // ENOTCONN
                LOG_T("spu", "SPU 0x%X event port %u value 0x%06X data 0x%X -> %s", t.id, spup, value & 0xFFFFFF, data,
                      q ? "queued" : "not connected");
                if (code < 64) {
                    std::lock_guard<std::mutex> lk(t.m);
                    t.in_mbox.push_back(res);
                }
                return true;
            }
            LOG_W("spu", "SPU 0x%X: SPU_WrOutIntrMbox 0x%08X (event flag ops) not implemented", t.id, value);
            std::lock_guard<std::mutex> lk(t.m);
            t.in_mbox.push_back(CELL_OK);
            return true;
        }
        default:
            LOG_E("spu", "SPU 0x%X: write 0x%X to unsupported channel %u at 0x%05X", t.id, value, ch, t.pc);
            return true;
    }
}

uint32_t spu_channel_count(SpuThread& t, uint32_t ch) {
    std::lock_guard<std::mutex> lk(t.m);
    switch (ch) {
        case SPU_RdInMbox: return (uint32_t)t.in_mbox.size();
        case SPU_WrOutMbox: return t.out_mbox_valid ? 0 : 1;
        case SPU_WrOutIntrMbox: return 1;
        case SPU_RdSigNotify1: return t.snr_valid[0] ? 1 : 0;
        case SPU_RdSigNotify2: return t.snr_valid[1] ? 1 : 0;
        case SPU_RdEventStat: return (pending_events(t) & t.event_mask) ? 1 : 0;
        case MFC_Cmd: return 16;
        default: return 1;
    }
}

bool spu_stop(SpuThread& t, uint32_t code) {
    switch (code) {
        case 0x102:  // sys_spu_thread_exit
            t.exit_status = t.out_mbox;
            return false;
        case 0x101:  // sys_spu_thread_group_exit
            t.exit_status = t.out_mbox;
            return false;
        case 0x103:  // sys_spu_thread_yield
            std::this_thread::yield();
            return true;
        default:
            if (code >= SPU_STOP_HLE_FIRST) return false;  // handled by the HLE kernel that started the thread
            LOG_E("spu", "SPU 0x%X: unsupported stop code 0x%X at 0x%05X (out_mbox 0x%X)", t.id, code, t.pc, t.out_mbox);
            return false;
    }
}
