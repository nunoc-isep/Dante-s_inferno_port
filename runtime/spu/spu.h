// Cell SPU emulation: thread state, local store, channels, MFC.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../kernel/kernel.h"

struct KEventQueue;
struct SpuGroup;

enum SpuOp : uint16_t {
#define SPU_OP(name, fmt, first, count) SPU_##name,
#include "spu_opcodes.inc"
#undef SPU_OP
    SPU_INVALID
};
extern SpuOp g_spu_decode[2048];  // 11-bit prefix -> op
extern const char* g_spu_op_names[];
void spu_init_decoder();

enum : uint32_t {  // channel numbers
    SPU_RdEventStat = 0, SPU_WrEventMask = 1, SPU_WrEventAck = 2, SPU_RdSigNotify1 = 3, SPU_RdSigNotify2 = 4,
    SPU_WrDec = 7, SPU_RdDec = 8, MFC_WrMSSyncReq = 9, SPU_RdEventMask = 11, MFC_RdTagMask = 12,
    SPU_RdMachStat = 13, SPU_WrSRR0 = 14, SPU_RdSRR0 = 15, MFC_LSA = 16, MFC_EAH = 17, MFC_EAL = 18,
    MFC_Size = 19, MFC_TagID = 20, MFC_Cmd = 21, MFC_WrTagMask = 22, MFC_WrTagUpdate = 23, MFC_RdTagStat = 24,
    MFC_RdListStallStat = 25, MFC_WrListStallAck = 26, MFC_RdAtomicStat = 27, SPU_WrOutMbox = 28,
    SPU_RdInMbox = 29, SPU_WrOutIntrMbox = 30,
};
enum : uint32_t {  // event bits
    SPU_EVENT_MS = 0x1000, SPU_EVENT_A = 0x800, SPU_EVENT_LR = 0x400, SPU_EVENT_S1 = 0x200, SPU_EVENT_S2 = 0x100,
    SPU_EVENT_LE = 0x80, SPU_EVENT_ME = 0x40, SPU_EVENT_TM = 0x20, SPU_EVENT_MB = 0x10, SPU_EVENT_QV = 0x8,
    SPU_EVENT_SN = 0x2, SPU_EVENT_TG = 0x1,
};

struct SpuThread {
    // architectural state
    alignas(16) PPCVR gpr[128];
    uint8_t* ls = nullptr;  // 256 KB, raw big-endian bytes
    uint32_t pc = 0;
    uint32_t srr0 = 0;

    // channels
    std::mutex m;
    std::condition_variable cv;
    std::deque<uint32_t> in_mbox;  // PPU -> SPU (max 4)
    bool out_mbox_valid = false;
    uint32_t out_mbox = 0;
    uint32_t snr[2] = {0, 0};
    bool snr_valid[2] = {false, false};
    uint32_t snr_or_mode[2] = {0, 0};
    uint32_t event_mask = 0, event_stat = 0;
    uint32_t dec_value = 0;
    uint64_t dec_start = 0;
    bool dec_armed = false;  // TM event fires once when the decrementer goes negative

    // MFC
    uint32_t mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
    uint32_t tag_mask = 0, atomic_stat = 0;
    bool rsv_valid = false;
    uint32_t rsv_addr = 0;
    uint32_t rsv_version = 0;  // line version at GETLLAR: any later store to the line loses the reservation
    alignas(16) uint8_t rsv_data[128];

    // lv2 side
    uint32_t id = 0;
    uint32_t spu_num = 0;
    SpuGroup* group = nullptr;
    std::string name;
    std::weak_ptr<KEventQueue> spup[64];
    std::atomic<bool> stop_req{false};
    bool exited = false;
    uint32_t exit_status = 0;
    uint64_t insn_count = 0;
    void* host_handle = nullptr;  // Win32 handle of the host thread running this SPU (perf accounting)
    uint32_t* profile = nullptr;  // --spu-profile: samples per LS word (pc >> 2), every 256 instructions
    uint8_t** native_states = nullptr;  // per recompiled image: validity of each code range (spu_native.cpp)
    uint32_t native_image = 0;
    uint64_t native_calls = 0;
    uint64_t native_insn = 0;
    uint64_t native_validations = 0;
    uint32_t ls_src[64] = {};
    void* ls_ov[64] = {};             // --spu-profile: overlay record (spu_lv2.cpp) that last filled each LS page
    volatile uint32_t native_pc = 0;  // block being executed by recompiled code (profiling)
    void* interp_pages = nullptr;     // --spu-profile: set of (LS page, source EA) pairs seen by the interpreter
    uint32_t* native_profile = nullptr;  // diagnostics: main-memory source of the last DMA into each 4 KB LS page  // instructions executed by recompiled code (also counted in insn_count)
    const char* state = "running";  // diagnostics: what the thread is blocked on
    std::thread host;

    SpuThread();
    ~SpuThread();
    uint32_t ls_read32(uint32_t a) const { return ppc_ld32(ls + (a & 0x3FFFC)); }
};

// Interpreter: runs until the thread stops (stop/exit). Returns the stop code.
uint32_t spu_run(SpuThread& t);
// Channel + MFC implementation (spu_channels.cpp). Return false to stop the thread.
bool spu_read_channel(SpuThread& t, uint32_t ch, uint32_t& value);
bool spu_write_channel(SpuThread& t, uint32_t ch, uint32_t value);
uint32_t spu_channel_count(SpuThread& t, uint32_t ch);
// stop-and-signal: returns true to continue execution after the stop instruction
bool spu_stop(SpuThread& t, uint32_t code);
std::string spu_disasm(uint32_t w, uint32_t pc);
// Recompiled SPU images (spu_native.cpp + generated code): available when pc is an entry of an
// image whose code is currently present in this thread's LS. spu_run_native returns 0 with t.pc
// set when execution leaves translated code, or SPU_EXIT(code) when the thread stops.
bool spu_native_available(SpuThread& t, uint32_t pc);
uint64_t spu_run_native(SpuThread& t, uint32_t pc);
void spu_native_ls_written(SpuThread& t, uint32_t lsa, uint32_t size);
void spu_dump_all();
void spu_register_host_thread(SpuThread& t);
void spu_perf_report(double seconds);
extern bool g_spu_profile;
// --spu-profile: DMA from the EBOOT image into LS (SPU code overlays); written to spu_overlays.txt
void spu_note_overlay(SpuThread& t, uint32_t ea, uint32_t lsa, uint32_t size);
void spu_note_overlay_exec(void* ov);
// Create an SPU thread object registered with the kernel (id, diagnostics) but not started.
std::shared_ptr<SpuThread> spu_create_thread(const std::string& name, uint32_t spu_num);
// Stop codes 0x3F00-0x3FFF are reserved for HLE kernels (e.g. SPURS): spu_run returns them silently.
constexpr uint32_t SPU_STOP_HLE_FIRST = 0x3F00;
