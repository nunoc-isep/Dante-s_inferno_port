// cellSpurs - compact HLE of the SPURS runtime.
//
// PPU side: attributes, initialize, workloads (add / ready count / shutdown /
// remove), event queues, thread ids, finalize.
// SPU side: one host thread per SPURS SPU runs a small C++ kernel that picks a
// ready workload (respecting max contention), copies its policy module to LS
// 0xA00 and starts it the way the real SPURS kernel does (RPCS3's
// spursKernelDispatchWorkload): $0 = exit address 0x818, $1 = 0x3FFB0,
// $3 = kernel context (LS 0x100), $4 = workload argument, $5 = poll status.
// A stop instruction at 0x818 hands control back to this kernel if the module
// returns. The system-service workload and the taskset/jobchain policy modules
// are not implemented (this game only uses custom policy modules).
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../kernel/kernel.h"
#include "../kernel/sync.h"
#include "../spu/spu.h"

namespace {

constexpr uint32_t kMaxWorkloads = 16;
constexpr uint32_t kExitAddr = 0x818, kSelectAddr = 0x290;
constexpr uint32_t kStopExit = SPU_STOP_HLE_FIRST + 1, kStopSelect = SPU_STOP_HLE_FIRST + 2;

enum class WklState { Free, Running, ShuttingDown, Shutdown };

struct Workload {
    WklState state = WklState::Free;
    uint32_t pm = 0, size = 0;
    uint64_t arg = 0;
    uint32_t min_contention = 0, max_contention = 1;
    uint32_t ready = 0, running = 0;
    std::string name;
};

struct Spurs {
    uint32_t addr = 0;
    uint32_t nspus = 0;
    std::string prefix;
    Workload wkl[kMaxWorkloads];
    std::vector<std::shared_ptr<SpuThread>> spus;
    std::vector<int> spu_wid;  // workload each SPU is running (-1 idle)
    std::mutex m;
    std::condition_variable cv;
    bool finalizing = false;
    uint32_t port_used = 0;  // bitmask of event ports 0..31 (dynamic ports from 0x10)
    uint64_t ports_hi_used = 0;
};

std::mutex g_spurs_mutex;
std::vector<std::shared_ptr<Spurs>> g_spurs;

std::shared_ptr<Spurs> find_spurs(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_spurs_mutex);
    for (auto& s : g_spurs)
        if (s->addr == addr) return s;
    return nullptr;
}

// Choose a workload for SPU `idx`: ready, running, below max contention; least loaded first.
int select_workload(Spurs& s) {
    int best = -1;
    for (uint32_t w = 0; w < kMaxWorkloads; ++w) {
        Workload& k = s.wkl[w];
        if (k.state != WklState::Running || !k.ready || k.running >= k.max_contention) continue;
        if (best < 0 || k.running < s.wkl[best].running) best = (int)w;
    }
    return best;
}

void spurs_kernel(std::shared_ptr<Spurs> s, uint32_t idx) {
    SpuThread& t = *s->spus[idx];
    LOG_I("spurs", "SPU kernel %u started (SPU thread 0x%X)", idx, t.id);
    for (;;) {
        int wid;
        {
            std::unique_lock<std::mutex> lk(s->m);
            s->cv.wait(lk, [&] { return s->finalizing || select_workload(*s) >= 0; });
            if (s->finalizing) break;
            wid = select_workload(*s);
            ++s->wkl[wid].running;
            s->spu_wid[idx] = wid;
        }
        Workload& w = s->wkl[wid];
        // kernel context (LS 0x100, layout of RPCS3's SpursKernelContext)
        memset(t.ls, 0, 0x40000);
        ppc_st64(t.ls + 0x1C0, s->addr);         // spurs
        ppc_st32(t.ls + 0x1C8, idx);             // spuNum
        ppc_st32(t.ls + 0x1CC, 31);              // dmaTagId
        ppc_st64(t.ls + 0x1D0, w.pm);            // wklCurrentAddr
        ppc_st32(t.ls + 0x1DC, (uint32_t)wid);   // wklCurrentId
        ppc_st32(t.ls + 0x1E0, kExitAddr);       // exitToKernelAddr
        ppc_st32(t.ls + 0x1E4, kSelectAddr);     // selectWorkloadAddr
        ppc_st32(t.ls + kExitAddr, kStopExit);   // "stop kStopExit" traps a module return
        ppc_st32(t.ls + kSelectAddr, kStopSelect);
        memcpy(t.ls + 0xA00, vm::ptr(w.pm), std::min<uint32_t>(w.size, 0x40000 - 0xA00));
        memset(t.gpr, 0, sizeof(t.gpr));
        VW(t.gpr[0], 0) = kExitAddr;
        VW(t.gpr[1], 0) = 0x3FFB0;
        VW(t.gpr[3], 0) = 0x100;
        t.gpr[4].u64[1] = w.arg;
        VW(t.gpr[5], 0) = 0;  // poll status
        t.pc = 0xA00;
        t.event_mask = t.event_stat = 0;
        t.rsv_valid = false;
        t.dec_armed = false;
        LOG_I("spurs", "SPU %u runs workload %u '%s' (pm 0x%08X, arg 0x%llX)", idx, wid, w.name.c_str(), w.pm,
              (unsigned long long)w.arg);
        uint32_t code;
        for (;;) {
            code = spu_run(t);
            if (code != kStopSelect) break;
            // Kernel select-workload service (cellSpursModulePollStatus calls it with $3 = 1):
            // returns (selected wid << 32) | poll status in $3's preferred doubleword. Keep the
            // current workload unless it is being shut down; 0x20 means "no workload" (exit).
            uint32_t sel;
            {
                std::lock_guard<std::mutex> lk(s->m);
                sel = (w.state == WklState::Running && !s->finalizing) ? (uint32_t)wid : 0x20;
            }
            t.gpr[3] = PPCVR{};
            t.gpr[3].u64[1] = (uint64_t)sel << 32;
            t.pc = VW(t.gpr[0], 0) & 0x3FFFC;  // return to the caller (bisl link register)
        }
        {
            std::lock_guard<std::mutex> lk(s->m);
            --w.running;
            s->spu_wid[idx] = -1;
            if (code == kStopExit) {
                LOG_I("spurs", "workload %u exited to the kernel on SPU %u", wid, idx);
            } else if (w.state == WklState::ShuttingDown || s->finalizing) {
                // stopped on request
            } else {
                LOG_E("spurs", "workload %u stopped unexpectedly on SPU %u (code 0x%X); disabling it", wid, idx, code);
                w.state = WklState::Shutdown;
            }
            if (w.state == WklState::ShuttingDown && w.running == 0) w.state = WklState::Shutdown;
            t.stop_req = false;
            s->cv.notify_all();
        }
    }
    LOG_I("spurs", "SPU kernel %u finished", idx);
}

void stop_workload_spus(Spurs& s, int wid) {
    for (size_t i = 0; i < s.spus.size(); ++i)
        if (s.spu_wid[i] == wid) s.spus[i]->stop_req = true;
}

}  // namespace

// ---------------------------------------------------------------- attributes / initialize
// CellSpursAttribute: u32 revision, sdkVersion, nSpus; s32 spuPriority, ppuPriority; bool exitIfNoWork (0x14),
//   char prefix[15] (0x15), u32 prefixSize (0x24), u32 flags (0x28), u32 container (0x2C), ...
HLE_FUNC(cellSpurs, _cellSpursAttributeInitialize) {
    uint32_t a = ARG32(0);
    memset(vm::ptr(a), 0, 0x200);
    vm::write32(a + 0x00, ARG32(1));
    vm::write32(a + 0x04, ARG32(2));
    vm::write32(a + 0x08, ARG32(3));
    vm::write32(a + 0x0C, ARG32(4));
    vm::write32(a + 0x10, ARG32(5));
    vm::write8(a + 0x14, ARG32(6) ? 1 : 0);
    RET(CELL_OK);
}
HLE_FUNC(cellSpurs, cellSpursAttributeSetNamePrefix) {
    uint32_t a = ARG32(0), n = ARG32(2) > 15 ? 15 : ARG32(2);
    memcpy(vm::ptr(a + 0x15), vm::ptr(ARG32(1)), n);
    vm::write32(a + 0x24, n);
    RET(CELL_OK);
}
HLE_FUNC(cellSpurs, cellSpursAttributeSetSpuThreadGroupType) { RET(CELL_OK); }

HLE_FUNC(cellSpurs, cellSpursInitializeWithAttribute) {
    uint32_t addr = ARG32(0), a = ARG32(1);
    auto s = std::make_shared<Spurs>();
    s->addr = addr;
    s->nspus = vm::read32(a + 8);
    if (s->nspus < 1 || s->nspus > 6) s->nspus = 4;
    s->prefix.assign((const char*)vm::ptr(a + 0x15), std::min<uint32_t>(vm::read32(a + 0x24), 15));
    memset(vm::ptr(addr), 0, 0x1000);  // CellSpurs (4 KB); the game only treats it as an opaque handle
    for (uint32_t i = 0; i < s->nspus; ++i) {
        s->spus.push_back(spu_create_thread(s->prefix + "SpursHdlr" + std::to_string(i), i));
        s->spu_wid.push_back(-1);
    }
    {
        std::lock_guard<std::mutex> lk(g_spurs_mutex);
        g_spurs.push_back(s);
    }
    for (uint32_t i = 0; i < s->nspus; ++i) std::thread(spurs_kernel, s, i).detach();
    LOG_I("spurs", "initialized '%s' at 0x%08X with %u SPUs", s->prefix.c_str(), addr, s->nspus);
    RET(CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursFinalize) {
    auto s = find_spurs(ARG32(0));
    if (!s) return (void)RET(0x80410711);  // CELL_SPURS_CORE_ERROR_INVAL
    std::lock_guard<std::mutex> lk(s->m);
    s->finalizing = true;
    for (auto& t : s->spus) t->stop_req = true;
    s->cv.notify_all();
    RET(CELL_OK);
}

// ---------------------------------------------------------------- workloads
// CellSpursWorkloadAttribute: u32 revision, sdkVersion, pm, size; u64 data (0x10); u8 priority[8] (0x18);
//   u32 minContention (0x20), maxContention (0x24), nameClass (0x28), nameInstance (0x2C), hook, hookArg
// _cellSpursWorkloadAttributeInitialize(attr, revision, sdk, pm, size, data, priority*, minCnt, maxCnt[stack])
HLE_FUNC(cellSpurs, _cellSpursWorkloadAttributeInitialize) {
    uint32_t a = ARG32(0);
    uint32_t max_cnt = vm::read32((uint32_t)ctx.r[1] + 0x70 + 4);  // 9th argument: caller's parameter save area
    memset(vm::ptr(a), 0, 0x200);
    vm::write32(a + 0x00, ARG32(1));
    vm::write32(a + 0x04, ARG32(2));
    vm::write32(a + 0x08, ARG32(3));
    vm::write32(a + 0x0C, ARG32(4));
    vm::write64(a + 0x10, ARG(5));
    if (ARG32(6)) memcpy(vm::ptr(a + 0x18), vm::ptr(ARG32(6)), 8);
    vm::write32(a + 0x20, ARG32(7));
    vm::write32(a + 0x24, max_cnt);
    RET(CELL_OK);
}
HLE_FUNC(cellSpurs, cellSpursWorkloadAttributeSetName) {
    vm::write32(ARG32(0) + 0x28, ARG32(1));
    vm::write32(ARG32(0) + 0x2C, ARG32(2));
    RET(CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursAddWorkloadWithAttribute) {
    auto s = find_spurs(ARG32(0));
    uint32_t wid_ptr = ARG32(1), a = ARG32(2);
    if (!s) return (void)RET(0x80410711);
    std::lock_guard<std::mutex> lk(s->m);
    for (uint32_t wid = 0; wid < kMaxWorkloads; ++wid) {
        Workload& w = s->wkl[wid];
        if (w.state != WklState::Free) continue;
        w = Workload{};
        w.state = WklState::Running;
        w.pm = vm::read32(a + 8);
        w.size = vm::read32(a + 12);
        w.arg = vm::read64(a + 0x10);
        w.min_contention = vm::read32(a + 0x20);
        w.max_contention = std::max<uint32_t>(1, vm::read32(a + 0x24));
        w.name = vm::read_cstr(vm::read32(a + 0x28), 64) + "/" + vm::read_cstr(vm::read32(a + 0x2C), 64);
        vm::write32(wid_ptr, wid);
        LOG_I("spurs", "workload %u '%s': pm 0x%08X size 0x%X arg 0x%llX contention %u..%u", wid, w.name.c_str(), w.pm,
              w.size, (unsigned long long)w.arg, w.min_contention, w.max_contention);
        return (void)RET(CELL_OK);
    }
    RET(0x80410701);  // CELL_SPURS_POLICY_MODULE_ERROR_AGAIN (no free slot)
}

HLE_FUNC(cellSpurs, cellSpursReadyCountStore) {
    auto s = find_spurs(ARG32(0));
    uint32_t wid = ARG32(1);
    if (!s || wid >= kMaxWorkloads) return (void)RET(0x80410711);
    std::lock_guard<std::mutex> lk(s->m);
    s->wkl[wid].ready = ARG32(2);
    s->cv.notify_all();
    RET(CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursWakeUp) {
    if (auto s = find_spurs(ARG32(0))) s->cv.notify_all();
    RET(CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursShutdownWorkload) {
    auto s = find_spurs(ARG32(0));
    uint32_t wid = ARG32(1);
    if (!s || wid >= kMaxWorkloads) return (void)RET(0x80410711);
    std::lock_guard<std::mutex> lk(s->m);
    Workload& w = s->wkl[wid];
    if (w.state == WklState::Running) {
        w.state = w.running ? WklState::ShuttingDown : WklState::Shutdown;
        stop_workload_spus(*s, (int)wid);
    }
    LOG_I("spurs", "shutdown workload %u", wid);
    RET(CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursWaitForWorkloadShutdown) {
    auto s = find_spurs(ARG32(0));
    uint32_t wid = ARG32(1);
    if (!s || wid >= kMaxWorkloads) return (void)RET(0x80410711);
    std::unique_lock<std::mutex> lk(s->m);
    s->cv.wait(lk, [&] { return s->wkl[wid].state == WklState::Shutdown || s->wkl[wid].state == WklState::Free; });
    RET(CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursRemoveWorkload) {
    auto s = find_spurs(ARG32(0));
    uint32_t wid = ARG32(1);
    if (!s || wid >= kMaxWorkloads) return (void)RET(0x80410711);
    std::lock_guard<std::mutex> lk(s->m);
    if (s->wkl[wid].running) return (void)RET(0x8041070A);  // CELL_SPURS_POLICY_MODULE_ERROR_BUSY
    s->wkl[wid] = Workload{};
    RET(CELL_OK);
}

// ---------------------------------------------------------------- event queues / threads
// cellSpursAttachLv2EventQueue(spurs, queue, port* (u8), isDynamic)
HLE_FUNC(cellSpurs, cellSpursAttachLv2EventQueue) {
    auto s = find_spurs(ARG32(0));
    auto q = event_queue_get(ARG32(1));
    uint32_t port_ptr = ARG32(2), dynamic = ARG32(3);
    if (!s || !q) return (void)RET(0x80410711);
    std::lock_guard<std::mutex> lk(s->m);
    uint32_t port = vm::read8(port_ptr);
    if (dynamic) {
        port = 0xFF;
        for (uint32_t p = 0x10; p < 0x40; ++p)
            if (!(s->ports_hi_used & (1ull << p))) {
                port = p;
                break;
            }
        if (port == 0xFF) return (void)RET(0x80410709);
    }
    if (port > 63) return (void)RET(0x80410702);
    s->ports_hi_used |= 1ull << port;
    for (auto& t : s->spus) t->spup[port] = q;
    vm::write8(port_ptr, (uint8_t)port);
    LOG_I("spurs", "event queue 0x%X attached on port %u", ARG32(1), port);
    RET(CELL_OK);
}

HLE_FUNC(cellSpurs, cellSpursDetachLv2EventQueue) {
    auto s = find_spurs(ARG32(0));
    uint32_t port = ARG32(1);
    if (!s || port > 63) return (void)RET(0x80410711);
    std::lock_guard<std::mutex> lk(s->m);
    s->ports_hi_used &= ~(1ull << port);
    for (auto& t : s->spus) t->spup[port].reset();
    RET(CELL_OK);
}

// cellSpursGetSpuThreadId(spurs, thread_ids*, count* (in: capacity, out: count))
HLE_FUNC(cellSpurs, cellSpursGetSpuThreadId) {
    auto s = find_spurs(ARG32(0));
    if (!s) return (void)RET(0x80410711);
    uint32_t cap = vm::read32(ARG32(2)), n = std::min<uint32_t>(cap, (uint32_t)s->spus.size());
    for (uint32_t i = 0; i < n; ++i) vm::write32(ARG32(1) + 4 * i, s->spus[i]->id);
    vm::write32(ARG32(2), n);
    RET(CELL_OK);
}
