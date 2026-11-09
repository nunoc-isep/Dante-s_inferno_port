// lv2 SPU syscalls (169-194) and sys_spu_image_import / close.
#include <cstring>
#include <vector>

#include "../kernel/sync.h"
#include "spu.h"

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
static struct SpuDumpHookInstaller {
    SpuDumpHookInstaller() { g_spu_dump_hook = spu_dump_all; }
} g_spu_dump_hook_installer;

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

static void spu_host_thread(std::shared_ptr<SpuThread> t, std::shared_ptr<SpuGroup> g) {
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
