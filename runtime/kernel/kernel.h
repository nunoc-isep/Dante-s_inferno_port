// Host-side emulation of the PS3 LV2 kernel pieces the game needs.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../ppc_context.h"

// ---------------------------------------------------------------- logging
enum class LogLevel { Error, Warn, Info, Trace };
extern LogLevel g_log_level;
void log_msg(LogLevel lvl, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
#define LOG_E(tag, ...) log_msg(LogLevel::Error, tag, __VA_ARGS__)
#define LOG_W(tag, ...) log_msg(LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_I(tag, ...) log_msg(LogLevel::Info, tag, __VA_ARGS__)
#define LOG_T(tag, ...) \
    do { if (g_log_level >= LogLevel::Trace) log_msg(LogLevel::Trace, tag, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------- error codes (CELL_*)
enum : uint32_t {
    CELL_OK = 0,
    CELL_EAGAIN = 0x80010001,
    CELL_EINVAL = 0x80010002,
    CELL_ENOSYS = 0x80010003,
    CELL_ENOMEM = 0x80010004,
    CELL_ESRCH = 0x80010005,
    CELL_ENOENT = 0x80010006,
    CELL_EDEADLK = 0x80010008,
    CELL_EPERM = 0x80010009,
    CELL_EBUSY = 0x8001000A,
    CELL_ETIMEDOUT = 0x8001000B,
    CELL_EFAULT = 0x8001000D,
    CELL_ESTAT = 0x8001000F,
    CELL_EALIGN = 0x80010010,
    CELL_EEXIST = 0x80010014,
    CELL_EISDIR = 0x80010012,
    CELL_ENOTDIR = 0x80010013,
    CELL_EBADF = 0x8001002A,
    CELL_ECANCELED = 0x8001000C,
};

// ---------------------------------------------------------------- guest memory
namespace vm {
extern uint8_t* g_base;

enum class Area { Main, User, Kernel, Mmapper, Video, Stack, Count };

bool init();
// Commit host pages for a guest range (idempotent).
void commit(uint32_t addr, uint32_t size);
uint32_t alloc(Area area, uint32_t size, uint32_t align = 0x10000);
bool free(Area area, uint32_t addr);
uint32_t area_base(Area area);
uint32_t area_size(Area area);
// Allocate exactly at addr inside an area (used for fixed mappings).
bool alloc_fixed(Area area, uint32_t addr, uint32_t size);

inline uint8_t* ptr(uint32_t a) { return g_base + a; }
inline uint8_t read8(uint32_t a) { return g_base[a]; }
inline uint16_t read16(uint32_t a) { return ppc_ld16(g_base + a); }
inline uint32_t read32(uint32_t a) { return ppc_ld32(g_base + a); }
inline uint64_t read64(uint32_t a) { return ppc_ld64(g_base + a); }
inline void write8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void write16(uint32_t a, uint16_t v) { ppc_st16(g_base + a, v); }
inline void write32(uint32_t a, uint32_t v) { ppc_st32(g_base + a, v); }
inline void write64(uint32_t a, uint64_t v) { ppc_st64(g_base + a, v); }
std::string read_cstr(uint32_t a, size_t max = 4096);
// Copy a host string into guest memory (NUL terminated).
void write_cstr(uint32_t a, const std::string& s);
}  // namespace vm

// ---------------------------------------------------------------- process / ELF
struct ProcessInfo {
    uint32_t entry_opd;
    uint32_t toc;
    uint32_t tls_addr, tls_filesz, tls_memsz;
    uint32_t primary_prio = 1001;
    uint32_t primary_stacksize = 0x100000;
    uint32_t malloc_pagesize = 0x100000;
    uint32_t sdk_version;
};
extern ProcessInfo g_process;
bool load_elf(const std::string& path);

// ---------------------------------------------------------------- threads
struct GuestThread;
GuestThread* current_thread();
PPCContext& current_ctx();
uint32_t thread_id(GuestThread* t);
// Create a guest PPU thread running the function described by `entry_opd`.
uint32_t thread_create(uint32_t entry_opd, uint64_t arg, int prio, uint32_t stack_size, uint64_t flags,
                       const std::string& name, bool is_main = false);
PPCContext* thread_context(uint32_t thread_id);
// Run the main thread and block until the process exits; returns exit code.
int run_main_thread(uint32_t thread_id);
[[noreturn]] void thread_exit(uint64_t status);
[[noreturn]] void process_exit(int code);
int thread_join(uint32_t id, uint64_t* status);
int thread_detach(uint32_t id);
uint32_t tls_alloc_for_thread();  // returns r13 value
void dump_trace(PPCContext& ctx);
void dump_all_threads();

// Call a guest function by code address on the current thread (used by HLE callbacks).
void call_guest(PPCContext& ctx, uint32_t code_addr);
// Call a guest function through its OPD descriptor (sets r2 to the TOC).
void call_guest_opd(PPCContext& ctx, uint32_t opd_addr);

// ---------------------------------------------------------------- HLE callbacks
// Guest code sometimes needs to call back into host code (e.g. the GCM command
// buffer callback). hle_make_callback returns the guest address of an OPD whose
// code address is a synthetic value that ppc_call_indirect routes to `fn`.
uint32_t hle_make_callback(PPCFunc fn);
PPCFunc hle_callback_lookup(uint32_t code_addr);

// ---------------------------------------------------------------- syscalls
typedef void (*SyscallFn)(PPCContext& ctx);
void syscall_register(uint32_t num, const char* name, SyscallFn fn);
struct SyscallRegistrar {
    SyscallRegistrar(uint32_t n, const char* name, SyscallFn fn) { syscall_register(n, name, fn); }
};
#define SYSCALL(num, name)                                                       \
    static void sys_##name(PPCContext& ctx);                                     \
    static SyscallRegistrar reg_sys_##name(num, #name, sys_##name);              \
    static void sys_##name(PPCContext& ctx)

// ---------------------------------------------------------------- virtual file system
// Map a guest path (/dev_bdvd/..., /app_home/..., /dev_hdd0/...) to a host path.
std::string vfs_host_path(const std::string& guest);
void vfs_init(const std::string& game_root, const std::string& user_root);

// ---------------------------------------------------------------- argument helpers
#define ARG(n) (ctx.r[3 + (n)])
#define ARG32(n) ((uint32_t)ctx.r[3 + (n)])
#define RET(v) (ctx.r[3] = (uint64_t)(int64_t)(int32_t)(uint32_t)(v))
