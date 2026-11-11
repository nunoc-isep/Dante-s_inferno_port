#include <windows.h>

// lv2 SPU syscalls (169-194) and sys_spu_image_import / close.
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <tuple>
#include <string>
#include <vector>

#include "../kernel/sync.h"
#include "spu.h"
#include "spu_native.h"

struct SpuGroup {
    uint32_t id = 0;
    std::string name;
    uint32_t num = 0;
    int prio = 0;
    std::vector<std::shared_ptr<SpuThread>> threads;
    std::mutex m;
    std::condition_variable cv;
    bool started = false, finished = false;
    uint32_t running = 0;
    uint32_t join_cause = 0, exit_status = 0;
};

static std::mutex g_spu_list_mutex;
static std::vector<std::weak_ptr<SpuThread>> g_spu_list;

extern void (*g_spu_dump_hook)();
extern void (*g_spu_perf_hook)(double);
void spu_perf_report(double seconds);
static void write_host_profile();
static struct SpuDumpHookInstaller {
    SpuDumpHookInstaller() {
        g_spu_dump_hook = spu_dump_all;
        g_spu_perf_hook = spu_perf_report;
    }
} g_spu_dump_hook_installer;

static uint64_t thread_cpu_100ns(void* h) {
    FILETIME c, e, k, u;
    if (!h || !GetThreadTimes((HANDLE)h, &c, &e, &k, &u)) return 0;
    return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

// --perf: per-SPU CPU usage and instruction rate since the previous call
void spu_perf_report(double seconds) {
    write_host_profile();
    static std::map<SpuThread*, std::pair<uint64_t, uint64_t>> last;  // cpu, instructions
    static std::map<SpuThread*, uint64_t> last_native;
    std::lock_guard<std::mutex> lk(g_spu_list_mutex);
    for (auto& w : g_spu_list) {
        auto t = w.lock();
        if (!t || t->exited) continue;
        uint64_t cpu = thread_cpu_100ns(t->host_handle), ins = t->insn_count, nat = t->native_insn;
        auto& l = last[t.get()];
        double pct = 100.0 * (cpu - l.first) / (seconds * 1e7);
        double interp = (ins - l.second) / seconds / 1e6, native = (nat - last_native[t.get()]) / seconds / 1e6;
        last_native[t.get()] = nat;
        LOG_I("perf", "  SPU %-34s CPU %5.1f%%  %7.1f M instr/s interpreted, %7.1f M native  (%s)", t->name.c_str(), pct,
              interp, native, t->state);
        if (t->native_states)
            for (uint32_t i = 0; i < g_spu_image_count; ++i) {
                uint8_t* st = t->native_states[i];
                if (!st) continue;
                uint32_t n[3] = {0, 0, 0};
                for (uint32_t r = 0; r < g_spu_images[i].range_count; ++r) ++n[st[r] < 3 ? st[r] : 0];
                LOG_I("perf", "      image %s: ranges valid %u, invalid %u, unchecked %u", g_spu_images[i].name, n[1], n[2], n[0]);
            }
        static std::map<SpuThread*, uint64_t> last_val;
        if (t->native_validations) {
            LOG_I("perf", "      range validations: %.0f/s", (t->native_validations - last_val[t.get()]) / seconds);
            last_val[t.get()] = t->native_validations;
        }
        l = {cpu, ins};
        if (t->profile) {
            // where the time went: per 0x400-byte LS region, then the hottest instructions
            std::vector<std::pair<uint32_t, uint32_t>> hot;
            uint64_t total = 0, kernel = 0;
            for (uint32_t i = 0; i < 0x10000; ++i) {
                uint32_t n = t->profile[i];
                if (!n) continue;
                total += n;
                if (i * 4 >= 0x3AE80) kernel += n;
                hot.push_back({n, i * 4});
            }
            std::sort(hot.rbegin(), hot.rend());
            std::string top;
            char b[32];
            for (size_t k = 0; k < hot.size() && k < 12; ++k) {
                uint32_t pc = hot[k].second;
                char bb[64];
                snprintf(bb, sizeof(bb), " %05X(ea %08X):%.1f%%", pc, t->ls_src[pc >> 12] + (pc & 0xFFF),
                         100.0 * hot[k].first / std::max<uint64_t>(total, 1));
                top += bb;
            }
            if (t->interp_pages) {
                auto& m = *(std::map<uint64_t, uint32_t>*)t->interp_pages;
                std::string pg;
                for (auto& [k, n] : m)
                    if (n > 20) {
                        char bb[48];
                        snprintf(bb, sizeof(bb), " LS%05X<-%08X:%u", (uint32_t)(k >> 32) << 12, (uint32_t)k, n);
                        pg += bb;
                    }
                LOG_I("perf", "      interpreted pages (LS page <- source EA : samples):%s", pg.c_str());
                m.clear();
            }
            LOG_I("perf", "      interpreted: kernel (>=0x3AE80) %.1f%% of samples; hottest:%s", 100.0 * kernel / std::max<uint64_t>(total, 1),
                  top.c_str());
            if (t->native_profile) {
                std::vector<std::pair<uint32_t, uint32_t>> nh;
                uint64_t nt = 0;
                for (uint32_t i = 0; i < 0x10000; ++i)
                    if (t->native_profile[i]) {
                        nt += t->native_profile[i];
                        nh.push_back({t->native_profile[i], i * 4});
                    }
                std::sort(nh.rbegin(), nh.rend());
                std::string ns;
                for (size_t k = 0; k < nh.size() && k < 14; ++k) {
                    char bb[40];
                    snprintf(bb, sizeof(bb), " %05X:%.1f%%", nh[k].second, 100.0 * nh[k].first / std::max<uint64_t>(nt, 1));
                    ns += bb;
                }
                LOG_I("perf", "      native blocks (sampled):%s", ns.c_str());
                static std::map<uint32_t, int> nd;
                if (nd[t->id]++ == 1) {
                    char fn[64];
                    snprintf(fn, sizeof(fn), "spu_nprof_%08X.bin", t->id);
                    if (FILE* f = fopen(fn, "wb")) {
                        fwrite(t->native_profile, 4, 0x10000, f);
                        fclose(f);
                    }
                }
                memset(t->native_profile, 0, 0x10000 * 4);
            }
            // also save the local store and the raw profile once, for offline analysis
            static std::map<uint32_t, int> dumped;
            if (dumped[t->id]++ == 1) {
                char fn[64];
                snprintf(fn, sizeof(fn), "spu_ls_%08X.bin", t->id);
                if (FILE* f = fopen(fn, "wb")) {
                    fwrite(t->ls, 1, 0x40000, f);
                    fclose(f);
                }
                snprintf(fn, sizeof(fn), "spu_prof_%08X.bin", t->id);
                if (FILE* f = fopen(fn, "wb")) {
                    fwrite(t->profile, 4, 0x10000, f);
                    fclose(f);
                }
            }
            memset(t->profile, 0, 0x10000 * 4);
        }
    }
}

void spu_dump_all() {
    std::lock_guard<std::mutex> lk(g_spu_list_mutex);
    for (auto& w : g_spu_list) {
        auto t = w.lock();
        if (!t) continue;
        fprintf(stderr, "SPU thread 0x%X '%s'%s: pc 0x%05X, %llu instructions, %s\n", t->id, t->name.c_str(),
                t->exited ? " (exited)" : "", t->pc, (unsigned long long)t->insn_count, t->state);
    }
    fflush(stderr);
}

std::shared_ptr<SpuThread> spu_create_thread(const std::string& name, uint32_t spu_num) {
    auto t = std::make_shared<SpuThread>();
    t->name = name;
    t->spu_num = spu_num;
    t->id = kobj_add(t);
    std::lock_guard<std::mutex> lk(g_spu_list_mutex);
    g_spu_list.push_back(t);
    return t;
}

enum : uint32_t {
    SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT = 1,
    SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT = 2,
    SYS_SPU_THREAD_GROUP_JOIN_TERMINATED = 4,
};

SYSCALL(169, sys_spu_initialize) {
    LOG_I("spu", "sys_spu_initialize(max_usable_spu %u, max_raw_spu %u)", ARG32(0), ARG32(1));
    RET(CELL_OK);
}

// sys_spu_thread_group_create(id*, num, prio, attr*)  attr: u32 nsize, u32 name, s32 type, u32 ct
SYSCALL(170, sys_spu_thread_group_create) {
    auto g = std::make_shared<SpuGroup>();
    g->num = ARG32(1);
    g->prio = (int)ARG32(2);
    uint32_t attr = ARG32(3);
    if (attr) g->name = vm::read_cstr(vm::read32(attr + 4), 64);
    g->threads.resize(8);  // slots are SPU numbers 0-7, independent of the group size (as in lv2)
    g->id = kobj_add(g);
    vm::write32(ARG32(0), g->id);
    LOG_I("spu", "group 0x%X '%s' created (%u threads, prio %d)", g->id, g->name.c_str(), g->num, g->prio);
    RET(CELL_OK);
}

SYSCALL(171, sys_spu_thread_group_destroy) { RET(kobj_remove(ARG32(0)) ? CELL_OK : CELL_ESRCH); }

// sys_spu_thread_initialize(thread*, group, spu_num, img*, attr*, arg*)
//   img: u32 type, u32 entry_point, u32 segs, s32 nsegs
//   segment (16 bytes): s32 type (1 copy, 2 fill, 4 info), u32 ls, u32 size, u32 addr/value
//   arg: u64 arg1..arg4 -> $3..$6 (preferred doubleword)
SYSCALL(172, sys_spu_thread_initialize) {
    auto g = kobj_get<SpuGroup>(ARG32(1));
    uint32_t num = ARG32(2), img = ARG32(3), attr = ARG32(4), arg = ARG32(5);
    if (!g || num >= 8) {
        LOG_W("spu", "sys_spu_thread_initialize(group 0x%X, slot %u): %s", ARG32(1), num, g ? "bad slot" : "no such group");
        return (void)RET(CELL_ESRCH);
    }
    auto t = std::make_shared<SpuThread>();
    t->group = g.get();
    t->spu_num = num;
    if (attr) t->name = vm::read_cstr(vm::read32(attr), 64);
    uint32_t entry = vm::read32(img + 4), segs = vm::read32(img + 8), nsegs = vm::read32(img + 12);
    for (uint32_t i = 0; i < nsegs; ++i) {
        uint32_t s = segs + 16 * i;
        uint32_t type = vm::read32(s), ls = vm::read32(s + 4), size = vm::read32(s + 8), src = vm::read32(s + 12);
        ls &= 0x3FFFF;
        if (ls + size > 0x40000) size = 0x40000 - ls;
        if (type == 1) memcpy(t->ls + ls, vm::ptr(src), size);
        else if (type == 2)
            for (uint32_t k = 0; k + 4 <= size; k += 4) ppc_st32(t->ls + ls + k, src);
    }
    t->pc = entry & 0x3FFFC;
    VW(t->gpr[1], 0) = 0x3FFF0;  // stack pointer
    for (int k = 0; k < 4; ++k) t->gpr[3 + k].u64[1] = arg ? vm::read64(arg + 8 * k) : 0;
    t->id = kobj_add(t);
    g->threads[num] = t;
    {
        std::lock_guard<std::mutex> lk(g_spu_list_mutex);
        g_spu_list.push_back(t);
    }
    vm::write32(ARG32(0), t->id);
    LOG_I("spu", "thread 0x%X '%s' in group 0x%X slot %u: entry 0x%05X, %u segments, args 0x%llX 0x%llX", t->id,
          t->name.c_str(), g->id, num, entry, nsegs, (unsigned long long)t->gpr[3].u64[1],
          (unsigned long long)t->gpr[4].u64[1]);
    RET(CELL_OK);
}

bool g_spu_profile = false;

// --spu-profile host sampling: x86 instruction pointers of busy SPU threads, as offsets into the
// exe (symbolise with tools/host_prof.py). Written to host_prof.txt by spu_perf_report.
static std::mutex g_host_prof_mutex;
static std::map<uint64_t, uint32_t> g_host_prof;  // absolute RIP -> samples

// PPU threads to sample too: DANTE_HOSTPROF_PPU=<name substring> with --spu-profile
static std::mutex g_extra_mutex;
static std::vector<HANDLE> g_extra_threads;

void hostprof_add_thread(void* handle, const std::string& name) {
    const char* filter = getenv("DANTE_HOSTPROF_PPU");
    if (!g_spu_profile || !filter || name.find(filter) == std::string::npos) return;
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), (HANDLE)handle, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard<std::mutex> lk(g_extra_mutex);
    g_extra_threads.push_back(h);
}

static void sample_rip(HANDLE h) {
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL;
    bool ok = GetThreadContext(h, &c);
    ResumeThread(h);
    if (ok) {
        std::lock_guard<std::mutex> hl(g_host_prof_mutex);
        ++g_host_prof[c.Rip];
    }
}

static void native_sampler() {
    for (uint32_t iter = 0;; ++iter) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        if ((iter & 7) == 0) {
            std::lock_guard<std::mutex> lk(g_extra_mutex);
            for (HANDLE h : g_extra_threads) sample_rip(h);
        }
        std::lock_guard<std::mutex> lk(g_spu_list_mutex);
        for (auto& w : g_spu_list)
            if (auto t = w.lock()) {
                if (t->native_profile) ++t->native_profile[(t->native_pc >> 2) & 0xFFFF];
                static const char* spu_filter = getenv("DANTE_HOSTPROF_SPU") ? getenv("DANTE_HOSTPROF_SPU") : "Job";
                if ((iter & 7) == 0 && t->host_handle && t->name.find(spu_filter) != std::string::npos && !getenv("DANTE_HOSTPROF_PPU")) {
                    HANDLE h = (HANDLE)t->host_handle;
                    if (SuspendThread(h) != (DWORD)-1) {
                        CONTEXT c{};
                        c.ContextFlags = CONTEXT_CONTROL;
                        bool ok = GetThreadContext(h, &c);
                        ResumeThread(h);
                        if (ok) {
                            std::lock_guard<std::mutex> hl(g_host_prof_mutex);
                            ++g_host_prof[c.Rip];
                        }
                    }
                }
            }
    }
}

static std::mutex g_overlay_mutex;
struct OverlayInfo {
    uint32_t count = 0;
    volatile bool executed = false;  // the interpreter sampled a PC in a page this DMA filled
};
static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, OverlayInfo> g_overlays;  // (ea, lsa, size)

void spu_note_overlay(SpuThread& t, uint32_t ea, uint32_t lsa, uint32_t size) {
    OverlayInfo* ov;
    {
        std::lock_guard<std::mutex> lk(g_overlay_mutex);
        ov = &g_overlays[{ea, lsa, size}];  // std::map nodes never move
        ++ov->count;
    }
    // only pages the copy covers entirely, so a small data DMA sharing a page is not mistaken for code
    for (uint32_t p = (lsa + 0xFFF) >> 12; p < ((lsa + size) >> 12) && p < 64; ++p) t.ls_ov[p] = ov;
    if (size < 0x1000) t.ls_ov[lsa >> 12] = ov;  // small overlays: the page they start in
}

void spu_note_overlay_exec(void* ov) { ((OverlayInfo*)ov)->executed = true; }

// spu_overlays.txt: overlays code ran from; merge into data/spu_overlays.txt for tools/spu_recomp.py
static void write_overlays() {
    std::lock_guard<std::mutex> lk(g_overlay_mutex);
    if (g_overlays.empty()) return;
    if (FILE* f = fopen("spu_overlays.txt", "w")) {
        for (auto& [k, ov] : g_overlays)
            if (ov.executed)
                fprintf(f, "%08X %05X %05X %u\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), ov.count);
        fclose(f);
    }
}

static void write_host_profile() {
    write_overlays();
    std::lock_guard<std::mutex> hl(g_host_prof_mutex);
    if (g_host_prof.empty()) return;
    if (FILE* f = fopen("host_prof.txt", "w")) {
        const uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
        std::map<std::string, uint32_t> dll;  // samples outside the exe, by "module!nearest export"
        for (auto& [rip, n] : g_host_prof) {
            HMODULE m = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)rip, &m);
            if (!m || (uintptr_t)m == exe) {
                fprintf(f, "%08X %u\n", (uint32_t)(rip - exe), n);
                continue;
            }
            char path[MAX_PATH] = "?";
            GetModuleFileNameA(m, path, MAX_PATH);
            const char* mod = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
            auto* b = (const uint8_t*)m;
            auto* nt = (const IMAGE_NT_HEADERS*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
            auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            std::string best = "?";
            uint32_t best_rva = 0, rva = (uint32_t)(rip - (uintptr_t)m);
            if (dir.VirtualAddress) {  // nearest export at or below the sample
                auto* ex = (const IMAGE_EXPORT_DIRECTORY*)(b + dir.VirtualAddress);
                auto* names = (const uint32_t*)(b + ex->AddressOfNames);
                auto* ords = (const uint16_t*)(b + ex->AddressOfNameOrdinals);
                auto* funcs = (const uint32_t*)(b + ex->AddressOfFunctions);
                for (uint32_t i = 0; i < ex->NumberOfNames; ++i) {
                    uint32_t fr = funcs[ords[i]];
                    if (fr <= rva && fr >= best_rva) {
                        best_rva = fr;
                        best = (const char*)(b + names[i]);
                    }
                }
            }
            dll[std::string(mod) + "!" + best] += n;
        }
        for (auto& [k, n] : dll) fprintf(f, "DLL %s %u\n", k.c_str(), n);
        fclose(f);
    }
}

void spu_register_host_thread(SpuThread& t) {
    if (g_spu_profile && !t.profile) {
        t.profile = new uint32_t[0x10000]();
        t.native_profile = new uint32_t[0x10000]();
        static std::atomic<bool> started{false};  // (std::call_once needs TLS support this toolchain lacks)
        if (!started.exchange(true)) std::thread(native_sampler).detach();
    }
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    t.host_handle = h;
}

static void spu_host_thread(std::shared_ptr<SpuThread> t, std::shared_ptr<SpuGroup> g) {
    spu_register_host_thread(*t);
    uint32_t code = spu_run(*t);
    t->exited = true;
    LOG_I("spu", "thread 0x%X '%s' stopped (code 0x%X, status 0x%X) after %llu instructions", t->id, t->name.c_str(),
          code, t->exit_status, (unsigned long long)t->insn_count);
    std::lock_guard<std::mutex> lk(g->m);
    if (code == 0x101) {
        g->join_cause = SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT;
        g->exit_status = t->exit_status;
        for (auto& o : g->threads)
            if (o && o != t) o->stop_req = true;
    }
    if (--g->running == 0) {
        if (!g->join_cause) g->join_cause = SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT;
        g->finished = true;
        g->cv.notify_all();
    }
}

SYSCALL(173, sys_spu_thread_group_start) {
    auto g = kobj_get<SpuGroup>(ARG32(0));
    if (!g) return (void)RET(CELL_ESRCH);
    std::lock_guard<std::mutex> lk(g->m);
    g->started = true;
    g->finished = false;
    g->join_cause = 0;
    for (auto& t : g->threads) {
        if (!t) continue;
        ++g->running;
        t->host = std::thread(spu_host_thread, t, g);
        t->host.detach();
    }
    LOG_I("spu", "group 0x%X started (%u threads)", g->id, g->running);
    RET(CELL_OK);
}

SYSCALL(177, sys_spu_thread_group_terminate) {
    auto g = kobj_get<SpuGroup>(ARG32(0));
    if (!g) return (void)RET(CELL_ESRCH);
    std::lock_guard<std::mutex> lk(g->m);
    g->join_cause = SYS_SPU_THREAD_GROUP_JOIN_TERMINATED;
    g->exit_status = ARG32(1);
    for (auto& t : g->threads)
        if (t) t->stop_req = true;
    RET(CELL_OK);
}

// sys_spu_thread_group_join(id, cause*, status*)
SYSCALL(178, sys_spu_thread_group_join) {
    auto g = kobj_get<SpuGroup>(ARG32(0));
    if (!g) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(g->m);
    g->cv.wait(lk, [&] { return g->finished; });
    if (ARG32(1)) vm::write32(ARG32(1), g->join_cause);
    if (ARG32(2)) vm::write32(ARG32(2), g->exit_status);
    RET(CELL_OK);
}

// sys_spu_thread_write_ls(id, lsa, value u64, type) / read_ls(id, lsa, value*, type)
SYSCALL(181, sys_spu_thread_write_ls) {
    auto t = kobj_get<SpuThread>(ARG32(0));
    if (!t) return (void)RET(CELL_ESRCH);
    uint32_t lsa = ARG32(1) & 0x3FFFF, type = ARG32(3);
    uint64_t v = ARG(2);
    spu_native_ls_written(*t, lsa, type);
    switch (type) {
        case 1: t->ls[lsa] = (uint8_t)v; break;
        case 2: ppc_st16(t->ls + lsa, (uint16_t)v); break;
        case 4: ppc_st32(t->ls + lsa, (uint32_t)v); break;
        case 8: ppc_st64(t->ls + lsa, v); break;
        default: return (void)RET(CELL_EINVAL);
    }
    RET(CELL_OK);
}
SYSCALL(182, sys_spu_thread_read_ls) {
    auto t = kobj_get<SpuThread>(ARG32(0));
    if (!t) return (void)RET(CELL_ESRCH);
    uint32_t lsa = ARG32(1) & 0x3FFFF, type = ARG32(3);
    uint64_t v;
    switch (type) {
        case 1: v = t->ls[lsa]; break;
        case 2: v = ppc_ld16(t->ls + lsa); break;
        case 4: v = ppc_ld32(t->ls + lsa); break;
        case 8: v = ppc_ld64(t->ls + lsa); break;
        default: return (void)RET(CELL_EINVAL);
    }
    vm::write64(ARG32(2), v);
    RET(CELL_OK);
}

// sys_spu_thread_write_snr(id, number, value)
SYSCALL(184, sys_spu_thread_write_snr) {
    auto t = kobj_get<SpuThread>(ARG32(0));
    if (!t) return (void)RET(CELL_ESRCH);
    uint32_t n = ARG32(1) & 1;
    std::lock_guard<std::mutex> lk(t->m);
    t->snr[n] = t->snr_or_mode[n] ? (t->snr[n] | ARG32(2)) : ARG32(2);
    t->snr_valid[n] = true;
    t->cv.notify_all();
    RET(CELL_OK);
}

SYSCALL(190, sys_spu_thread_write_spu_mb) {
    auto t = kobj_get<SpuThread>(ARG32(0));
    if (!t) return (void)RET(CELL_ESRCH);
    std::lock_guard<std::mutex> lk(t->m);
    t->in_mbox.push_back(ARG32(1));
    t->cv.notify_all();
    RET(CELL_OK);
}

// sys_spu_thread_connect_event(id, eq, et, spup)  et: 1 = user event
SYSCALL(191, sys_spu_thread_connect_event) {
    auto t = kobj_get<SpuThread>(ARG32(0));
    auto q = event_queue_get(ARG32(1));
    uint32_t et = ARG32(2), spup = ARG32(3);
    if (!t || !q) return (void)RET(CELL_ESRCH);
    if (et != 1 || spup > 63) {
        LOG_W("spu", "connect_event: event type %u port %u not supported", et, spup);
        return (void)RET(CELL_EINVAL);
    }
    t->spup[spup] = q;
    LOG_I("spu", "thread 0x%X port %u -> event queue 0x%X", t->id, spup, ARG32(1));
    RET(CELL_OK);
}
SYSCALL(192, sys_spu_thread_disconnect_event) {
    auto t = kobj_get<SpuThread>(ARG32(0));
    if (!t) return (void)RET(CELL_ESRCH);
    if (ARG32(2) < 64) t->spup[ARG32(2)].reset();
    RET(CELL_OK);
}

// ---------------------------------------------------------------- sys_spu_image_import / close (sysPrxForUser)
// sys_spu_image_import(img*, src, type): build a segment list straight from the embedded SPU ELF
HLE_FUNC(sysPrxForUser, sys_spu_image_import) {
    uint32_t img = ARG32(0), src = ARG32(1);
    if (vm::read32(src) != 0x7F454C46) return (void)RET(CELL_EINVAL);
    uint32_t entry = vm::read32(src + 24), phoff = vm::read32(src + 28);
    uint16_t phnum = vm::read16(src + 44);
    uint32_t segs = vm::alloc(vm::Area::Kernel, 16 * 2 * phnum + 16, 0x10);
    uint32_t n = 0;
    for (uint32_t i = 0; i < phnum; ++i) {
        uint32_t p = src + phoff + 32 * i;
        if (vm::read32(p) != 1) continue;  // PT_LOAD
        uint32_t off = vm::read32(p + 4), va = vm::read32(p + 8), filesz = vm::read32(p + 16), memsz = vm::read32(p + 20);
        if (filesz) {
            uint32_t s = segs + 16 * n++;
            vm::write32(s, 1);
            vm::write32(s + 4, va);
            vm::write32(s + 8, filesz);
            vm::write32(s + 12, src + off);
        }
        if (memsz > filesz) {
            uint32_t s = segs + 16 * n++;
            vm::write32(s, 2);
            vm::write32(s + 4, va + filesz);
            vm::write32(s + 8, memsz - filesz);
            vm::write32(s + 12, 0);
        }
    }
    vm::write32(img + 0, 0);  // SYS_SPU_IMAGE_TYPE_USER
    vm::write32(img + 4, entry);
    vm::write32(img + 8, segs);
    vm::write32(img + 12, n);
    LOG_I("spu", "sys_spu_image_import(0x%08X): entry 0x%05X, %u segments", src, entry, n);
    RET(CELL_OK);
}
HLE_FUNC(sysPrxForUser, sys_spu_image_close) { RET(CELL_OK); }
