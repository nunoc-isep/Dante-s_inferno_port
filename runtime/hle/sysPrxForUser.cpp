// sysPrxForUser: TLS, lightweight mutexes, PPU threads, process helpers.
#include "../kernel/kernel.h"
#include "../kernel/sync.h"

// sys_initialize_tls(main_thread_id, tls_seg_addr, tls_seg_size, tls_mem_size)
HLE_FUNC(sysPrxForUser, sys_initialize_tls) {
    ctx.r[13] = tls_alloc_for_thread();
    LOG_I("hle", "sys_initialize_tls -> r13 = 0x%08llX", (unsigned long long)ctx.r[13]);
    RET(CELL_OK);
}

// ---------------------------------------------------------------- lwmutex
// sys_lwmutex_t (24 bytes): u32 owner, u32 waiter, u32 attribute, u32 recursive_count,
//                           u32 sleep_queue (our KMutex id), u32 pad
// sys_lwmutex_attribute_t: u32 protocol, u32 recursive (0x10 = recursive, 0x20 = not), char name[8]
static uint32_t self_id() { return thread_id(current_thread()); }

HLE_FUNC(sysPrxForUser, sys_lwmutex_create) {
    uint32_t lw = ARG32(0), attr = ARG32(1);
    uint32_t protocol = vm::read32(attr), recursive = vm::read32(attr + 4);
    auto m = std::make_shared<KMutex>();
    m->recursive = recursive == 0x10;
    uint32_t id = kobj_add(m);
    vm::write32(lw + 0, 0);
    vm::write32(lw + 4, 0);
    vm::write32(lw + 8, protocol | recursive);
    vm::write32(lw + 12, 0);
    vm::write32(lw + 16, id);
    vm::write32(lw + 20, 0);
    LOG_T("hle", "lwmutex create 0x%08X '%.8s' -> 0x%X", lw, (const char*)vm::ptr(attr + 8), id);
    RET(CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_destroy) {
    uint32_t lw = ARG32(0);
    kobj_remove(vm::read32(lw + 16));
    vm::write32(lw + 16, 0);
    RET(CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_lock) {
    uint32_t lw = ARG32(0);
    auto m = kobj_get<KMutex>(vm::read32(lw + 16));
    if (!m) return (void)RET(CELL_ESRCH);
    uint32_t r = m->lock(self_id(), ARG(1));
    if (r == CELL_OK) {
        vm::write32(lw + 0, self_id());
        vm::write32(lw + 12, m->count);
    }
    RET(r);
}

HLE_FUNC(sysPrxForUser, sys_lwmutex_unlock) {
    uint32_t lw = ARG32(0);
    auto m = kobj_get<KMutex>(vm::read32(lw + 16));
    if (!m) return (void)RET(CELL_ESRCH);
    uint32_t remaining = m->count ? m->count - 1 : 0;
    if (m->owner == self_id()) {
        vm::write32(lw + 12, remaining);
        if (!remaining) vm::write32(lw + 0, 0);
    }
    RET(m->unlock(self_id()));
}

// ---------------------------------------------------------------- threads / process
// sys_ppu_thread_create(thread_id* (u64), entry OPD, arg u64, prio, stacksize, flags u64, name)
HLE_FUNC(sysPrxForUser, sys_ppu_thread_create) {
    std::string name = vm::read_cstr(ARG32(6), 64);
    uint32_t id = thread_create(ARG32(1), ARG(2), (int)ARG32(3), ARG32(4), ARG(5), name);
    vm::write64(ARG32(0), id);
    RET(CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_ppu_thread_get_id) {
    vm::write64(ARG32(0), thread_id(current_thread()));
    RET(CELL_OK);
}

HLE_FUNC(sysPrxForUser, sys_ppu_thread_exit) { thread_exit(ARG(0)); }
HLE_FUNC(sysPrxForUser, sys_process_exit) { process_exit((int)ARG32(0)); }

HLE_FUNC(sysPrxForUser, sys_time_get_system_time) { ctx.r[3] = system_time_us(); }

HLE_FUNC(sysPrxForUser, sys_process_is_stack) {
    uint32_t a = ARG32(0);
    ctx.r[3] = (a >= vm::area_base(vm::Area::Stack) && a < vm::area_base(vm::Area::Stack) + vm::area_size(vm::Area::Stack));
}

HLE_FUNC(sysPrxForUser, sys_prx_register_library) { RET(CELL_OK); }
HLE_FUNC(sysPrxForUser, _sys_process_atexitspawn) { RET(CELL_OK); }
HLE_FUNC(sysPrxForUser, _sys_process_at_Exitspawn) { RET(CELL_OK); }
HLE_FUNC(sysPrxForUser, sys_prx_exitspawn_with_level) { RET(CELL_OK); }
