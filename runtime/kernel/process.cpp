// ELF loading, PPU threads, TLS, syscall dispatch and crash diagnostics.
#include <windows.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "kernel.h"

ProcessInfo g_process;

// ---------------------------------------------------------------- ELF loader
static uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static uint16_t be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

bool load_elf(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        LOG_E("loader", "cannot open %s", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    std::vector<uint8_t> d(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (d.size() < 64 || memcmp(d.data(), "\x7f" "ELF", 4) != 0) {
        LOG_E("loader", "%s is not an ELF (decrypt EBOOT.BIN with RPCS3 first)", path.c_str());
        return false;
    }
    uint32_t entry = (uint32_t)be64(&d[0x18]);
    uint64_t phoff = be64(&d[0x20]);
    uint16_t phentsize = be16(&d[0x36]), phnum = be16(&d[0x38]);
    for (int i = 0; i < phnum; ++i) {
        const uint8_t* p = &d[phoff + (size_t)i * phentsize];
        uint32_t type = be32(p);
        uint64_t off = be64(p + 8), va = be64(p + 16), filesz = be64(p + 32), memsz = be64(p + 40);
        if (type == 1 && memsz) {  // PT_LOAD
            if (!vm::alloc_fixed(vm::Area::Main, (uint32_t)va, (uint32_t)memsz)) {
                LOG_E("loader", "cannot map segment at 0x%08llX", (unsigned long long)va);
                return false;
            }
            memcpy(vm::ptr((uint32_t)va), &d[off], filesz);
            LOG_I("loader", "segment 0x%08llX size 0x%08llX", (unsigned long long)va, (unsigned long long)memsz);
        } else if (type == 7) {  // PT_TLS
            g_process.tls_addr = (uint32_t)va;
            g_process.tls_filesz = (uint32_t)filesz;
            g_process.tls_memsz = (uint32_t)memsz;
        } else if (type == 0x60000001 && filesz >= 0x20) {  // sys_process_param_t
            const uint8_t* q = &d[off];
            g_process.sdk_version = be32(q + 12);
            g_process.primary_prio = be32(q + 16);
            g_process.primary_stacksize = be32(q + 20);
            g_process.malloc_pagesize = be32(q + 24);
        }
    }
    g_process.entry_opd = entry;
    g_process.toc = vm::read32(entry + 4);
    LOG_I("loader", "entry OPD 0x%08X (code 0x%08X, TOC 0x%08X), TLS 0x%08X/0x%X/0x%X, stack 0x%X, prio %u, sdk 0x%08X",
          entry, vm::read32(entry), g_process.toc, g_process.tls_addr, g_process.tls_filesz, g_process.tls_memsz,
          g_process.primary_stacksize, g_process.primary_prio, g_process.sdk_version);
    return true;
}

// ---------------------------------------------------------------- threads
struct ThreadExit {
    uint64_t status;
};

struct GuestThread {
    uint32_t id = 0;
    PPCContext ctx{};
    std::string name;
    uint32_t entry_opd = 0;
    uint64_t arg = 0;
    uint32_t stack_addr = 0, stack_size = 0;
    bool is_main = false;
    HANDLE handle = nullptr;
    std::atomic<bool> finished{false};
    uint64_t exit_status = 0;
    bool detached = false;
};

static std::mutex g_threads_mutex;
static std::map<uint32_t, GuestThread*> g_threads;
static std::atomic<uint32_t> g_next_thread_id{0x01000000};
// Native Win32 TLS slot (safe to read from the vectored exception handler,
// unlike MinGW's emulated C++ thread_local).
static DWORD g_tls_slot = TlsAlloc();
static inline GuestThread* tls_current() { return (GuestThread*)TlsGetValue(g_tls_slot); }
#define t_current tls_current()

GuestThread* current_thread() { return t_current; }
PPCContext& current_ctx() { return t_current->ctx; }
uint32_t thread_id(GuestThread* t) { return t ? t->id : 0; }

uint32_t tls_alloc_for_thread() {
    if (!g_process.tls_memsz) return 0;
    uint32_t addr = vm::alloc(vm::Area::Kernel, g_process.tls_memsz + 0x30, 0x10);
    memset(vm::ptr(addr), 0, g_process.tls_memsz + 0x30);
    memcpy(vm::ptr(addr + 0x30), vm::ptr(g_process.tls_addr), g_process.tls_filesz);
    return addr + 0x7030;  // r13 convention: TLS block starts at r13 - 0x7000
}

void call_guest(PPCContext& ctx, uint32_t code_addr) { ppc_call_indirect(ctx, vm::g_base, code_addr); }

void call_guest_opd(PPCContext& ctx, uint32_t opd_addr) {
    uint64_t saved_toc = ctx.r[2];
    ctx.r[2] = vm::read32(opd_addr + 4);
    ppc_call_indirect(ctx, vm::g_base, vm::read32(opd_addr));
    ctx.r[2] = saved_toc;
}

static DWORD WINAPI thread_main(LPVOID param) {
    GuestThread* t = (GuestThread*)param;
    TlsSetValue(g_tls_slot, t);
    PPCContext& ctx = t->ctx;
    try {
        call_guest_opd(ctx, t->entry_opd);
        t->exit_status = ctx.r[3];
    } catch (const ThreadExit& e) {
        t->exit_status = e.status;
    }
    LOG_I("thread", "thread 0x%X '%s' exited (status 0x%llX)", t->id, t->name.c_str(),
          (unsigned long long)t->exit_status);
    t->finished = true;
    return 0;
}

uint32_t thread_create(uint32_t entry_opd, uint64_t arg, int prio, uint32_t stack_size, uint64_t flags,
                       const std::string& name, bool is_main) {
    GuestThread* t = new GuestThread();
    t->id = g_next_thread_id++;
    t->name = name;
    t->entry_opd = entry_opd;
    t->arg = arg;
    t->is_main = is_main;
    t->stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    if (t->stack_size < 0x4000) t->stack_size = 0x4000;
    t->stack_addr = vm::alloc(vm::Area::Stack, t->stack_size, 0x10000);
    PPCContext& ctx = t->ctx;
    ctx.thread = t;
    ctx.r[1] = t->stack_addr + t->stack_size - 0x70;
    vm::write64((uint32_t)ctx.r[1], 0);  // back chain
    ctx.r[3] = arg;
    ctx.r[13] = is_main ? 0 : tls_alloc_for_thread();
    ctx.fpscr = 0;
    ctx.vscr = 0x10000;  // NJ
    {
        std::lock_guard<std::mutex> lk(g_threads_mutex);
        g_threads[t->id] = t;
    }
    LOG_I("thread", "create 0x%X '%s' entry OPD 0x%08X arg 0x%llX prio %d stack 0x%X flags 0x%llX", t->id,
          name.c_str(), entry_opd, (unsigned long long)arg, prio, t->stack_size, (unsigned long long)flags);
    if (!is_main) {
        // 256 MB host stack reservation: guest recursion becomes host recursion.
        t->handle = CreateThread(nullptr, 256ull << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }
    return t->id;
}

static GuestThread* find_thread(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second;
}

PPCContext* thread_context(uint32_t id) {
    GuestThread* t = find_thread(id);
    return t ? &t->ctx : nullptr;
}

int run_main_thread(uint32_t id) {
    GuestThread* t = find_thread(id);
    t->handle = CreateThread(nullptr, 512ull << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(t->handle, INFINITE);
    return (int)t->exit_status;
}

void thread_exit(uint64_t status) { throw ThreadExit{status}; }

void process_exit(int code) {
    LOG_I("process", "exit(%d)", code);
    fflush(stdout);
    fflush(stderr);
    ExitProcess((UINT)code);
}

int thread_join(uint32_t id, uint64_t* status) {
    GuestThread* t = find_thread(id);
    if (!t) return CELL_ESRCH;
    if (t == t_current) return CELL_EDEADLK;
    WaitForSingleObject(t->handle, INFINITE);
    if (status) *status = t->exit_status;
    return CELL_OK;
}

int thread_detach(uint32_t id) {
    GuestThread* t = find_thread(id);
    if (!t) return CELL_ESRCH;
    t->detached = true;
    return CELL_OK;
}

void (*g_spu_dump_hook)() = nullptr;

void dump_all_threads() {
    if (g_spu_dump_hook) g_spu_dump_hook();
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    fprintf(stderr, "\n=== guest threads ===\n");
    for (auto& [id, t] : g_threads) {
        fprintf(stderr, "thread 0x%X '%s'%s\n", id, t->name.c_str(), t->finished ? " (finished)" : "");
        if (!t->finished) dump_trace(t->ctx);
    }
    fflush(stderr);
}

void dump_trace(PPCContext& ctx) {
    fprintf(stderr, "  last guest functions entered (oldest first):\n   ");
    for (uint32_t k = 0; k < 64; ++k) {
        uint32_t a = ctx.trace[(ctx.trace_pos + k) & 63];
        if (a) fprintf(stderr, " %08X", a);
    }
    fprintf(stderr, "\n  r1=%08llX r2=%08llX r3=%016llX r4=%016llX lr=%08llX ctr=%08llX\n",
            (unsigned long long)ctx.r[1], (unsigned long long)ctx.r[2], (unsigned long long)ctx.r[3],
            (unsigned long long)ctx.r[4], (unsigned long long)ctx.lr, (unsigned long long)ctx.ctr);
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  r%-2d %016llX  r%-2d %016llX  r%-2d %016llX  r%-2d %016llX\n", i,
                (unsigned long long)ctx.r[i], i + 1, (unsigned long long)ctx.r[i + 1], i + 2,
                (unsigned long long)ctx.r[i + 2], i + 3, (unsigned long long)ctx.r[i + 3]);
    // Guest call stack: PPC64 frames hold the back chain at 0(r1); each function
    // saves its return address in its caller's frame at +0x10.
    fprintf(stderr, "  guest call stack (return addresses):");
    uint32_t sa = vm::area_base(vm::Area::Stack), se = sa + vm::area_size(vm::Area::Stack);
    uint32_t frame = (uint32_t)ctx.r[1];
    for (int depth = 0; depth < 24; ++depth) {
        if (frame < sa || frame + 0x18 > se || (frame & 7)) break;
        uint32_t next = (uint32_t)vm::read64(frame);
        if (next <= frame || next < sa || next + 0x18 > se) break;
        fprintf(stderr, " %08X", (uint32_t)vm::read64(next + 0x10));
        frame = next;
    }
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------------- HLE callbacks
// Synthetic code addresses live at 0xE0000000+ (outside every guest area, never executed).
static constexpr uint32_t kCallbackBase = 0xE0000000, kMaxCallbacks = 1024;
static PPCFunc g_callbacks[kMaxCallbacks];
static std::atomic<uint32_t> g_num_callbacks{0};
static uint32_t g_callback_opds = 0;
static std::mutex g_callback_mutex;

uint32_t hle_make_callback(PPCFunc fn) {
    std::lock_guard<std::mutex> lk(g_callback_mutex);
    if (!g_callback_opds) g_callback_opds = vm::alloc(vm::Area::Kernel, kMaxCallbacks * 8, 0x1000);
    uint32_t n = g_num_callbacks;
    g_callbacks[n] = fn;
    g_num_callbacks = n + 1;
    uint32_t opd = g_callback_opds + n * 8;
    vm::write32(opd, kCallbackBase + n * 4);
    vm::write32(opd + 4, g_process.toc);
    return opd;
}

PPCFunc hle_callback_lookup(uint32_t code_addr) {
    if (code_addr < kCallbackBase) return nullptr;
    uint32_t n = (code_addr - kCallbackBase) / 4;
    return n < g_num_callbacks ? g_callbacks[n] : nullptr;
}

// ---------------------------------------------------------------- crash handler
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n*** host exception 0x%08lX ", code);
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        const char* op = ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read";
        if (a >= (uintptr_t)vm::g_base && a < (uintptr_t)vm::g_base + 0x100000000ull)
            fprintf(stderr, "(guest %s at 0x%08llX - unmapped guest memory)", op,
                    (unsigned long long)(a - (uintptr_t)vm::g_base));
        else
            fprintf(stderr, "(host %s at %p)", op, (void*)a);
    }
    fprintf(stderr, "\n  host thread %lu, rip %p\n", GetCurrentThreadId(), (void*)ep->ContextRecord->Rip);
    if (t_current) {
        fprintf(stderr, "  guest thread 0x%X '%s'\n", t_current->id, t_current->name.c_str());
        dump_trace(t_current->ctx);
    } else {
        fprintf(stderr, "  (not a guest thread)\n");
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);  // no atexit/cleanup: state is unknown
    return EXCEPTION_CONTINUE_SEARCH;
}

struct CrashHandlerInstaller {
    CrashHandlerInstaller() { AddVectoredExceptionHandler(1, crash_handler); }
} g_crash_handler_installer;

// ---------------------------------------------------------------- syscalls
struct SyscallEntry {
    const char* name;
    SyscallFn fn;
};
static SyscallEntry* g_syscalls() {
    static SyscallEntry table[1024];
    return table;
}

void syscall_register(uint32_t num, const char* name, SyscallFn fn) {
    if (num < 1024) g_syscalls()[num] = {name, fn};
}

void ppc_syscall(PPCContext& ctx, uint8_t*, uint32_t addr) {
    uint32_t n = (uint32_t)ctx.r[11];
    SyscallEntry& e = g_syscalls()[n & 1023];
    if (n < 1024 && e.fn) {
        LOG_T("lv2", "%s(0x%llX, 0x%llX, 0x%llX, 0x%llX) @%08X", e.name, (unsigned long long)ctx.r[3],
              (unsigned long long)ctx.r[4], (unsigned long long)ctx.r[5], (unsigned long long)ctx.r[6], addr);
        e.fn(ctx);
        return;
    }
    static std::mutex m;
    static std::map<uint32_t, int> seen;
    {
        std::lock_guard<std::mutex> lk(m);
        if (seen[n]++ < 3)
            LOG_W("lv2", "unimplemented syscall %u at 0x%08X (r3=0x%llX r4=0x%llX), returning CELL_OK", n, addr,
                  (unsigned long long)ctx.r[3], (unsigned long long)ctx.r[4]);
    }
    ctx.r[3] = CELL_OK;
}
