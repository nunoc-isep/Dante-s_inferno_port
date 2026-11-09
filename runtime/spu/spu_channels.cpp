// SPU channel interface: MFC DMA (incl. list and atomic reservation commands),
// mailboxes, signal notification, events, decrementer, stop-and-signal.
#include <chrono>
#include <cstring>

#include "../kernel/sync.h"
#include "spu.h"

static std::mutex g_rsv_mutex;  // serialises SPU atomic (128-byte reservation) operations

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
    } else {
        memcpy(t.ls + lsa, vm::ptr(ea), size);
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
            std::lock_guard<std::mutex> lk(g_rsv_mutex);
            memcpy(t.rsv_data, vm::ptr(line), 128);
            memcpy(t.ls + (lsa & 0x3FF80), t.rsv_data, 128);
            t.rsv_addr = line;
            t.rsv_valid = true;
            t.atomic_stat = MFC_GETLLAR_SUCCESS;
            break;
        }
        case 0xB4: {  // PUTLLC
            uint32_t line = ea & ~127u;
            std::lock_guard<std::mutex> lk(g_rsv_mutex);
            bool ok = t.rsv_valid && t.rsv_addr == line && memcmp(vm::ptr(line), t.rsv_data, 128) == 0;
            if (ok) memcpy(vm::ptr(line), t.ls + (lsa & 0x3FF80), 128);
            t.rsv_valid = false;
            t.atomic_stat = ok ? MFC_PUTLLC_SUCCESS : MFC_PUTLLC_FAILURE;
            break;
        }
        case 0xB0: case 0xB8: {  // PUTLLUC, PUTQLLUC
            uint32_t line = ea & ~127u;
            std::lock_guard<std::mutex> lk(g_rsv_mutex);
            memcpy(vm::ptr(line), t.ls + (lsa & 0x3FF80), 128);
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
    if (t.rsv_valid && memcmp(vm::ptr(t.rsv_addr), t.rsv_data, 128) != 0) {
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
            for (;;) {
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
                    t.cv.wait_for(lk, std::chrono::microseconds(50));
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
