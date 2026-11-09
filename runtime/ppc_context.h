// Guest CPU state and helper macros used by the recompiled PPU code.
// Guest memory is a flat 4 GB reservation at `base`; guest addresses are
// 32-bit (PS3 user space), data is big-endian.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#error "Generated code needs clang (uses __int128 and __builtin_*)"
#endif

struct PPCCRField {
    uint8_t lt, gt, eq, so;

    template <typename T>
    inline void compare(T a, T b, uint8_t xer_so) {
        lt = a < b;
        gt = a > b;
        eq = a == b;
        so = xer_so;
    }
    inline void compare_f(double a, double b) {
        lt = a < b;
        gt = a > b;
        eq = a == b;
        so = std::isnan(a) || std::isnan(b);  // FU (unordered)
    }
    inline void set_stcx(bool ok, uint8_t xer_so) {
        lt = gt = 0;
        eq = ok;
        so = xer_so;
    }
    inline uint32_t get() const { return (lt << 3) | (gt << 2) | (eq << 1) | so; }
    inline void set(uint32_t v) {
        lt = (v >> 3) & 1;
        gt = (v >> 2) & 1;
        eq = (v >> 1) & 1;
        so = v & 1;
    }
};

struct PPCXER {
    uint8_t so, ov, ca;
    inline uint64_t get() const { return ((uint64_t)so << 31) | ((uint64_t)ov << 30) | ((uint64_t)ca << 29); }
    inline void set(uint64_t v) {
        so = (v >> 31) & 1;
        ov = (v >> 30) & 1;
        ca = (v >> 29) & 1;
    }
};

union PPCFPR {
    double f64;
    uint64_t u64;
};

union alignas(16) PPCVR {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    float f32[4];
    uint64_t u64[2];
};

struct PPCContext {
    uint64_t r[32];
    PPCFPR f[32];
    PPCVR v[32];
    uint64_t lr, ctr;
    PPCXER xer;
    PPCCRField cr[8];
    uint32_t fpscr;
    uint32_t vscr;
    uint32_t vrsave;
    // load-reserve state (lwarx/ldarx)
    uint32_t reserve_addr;
    uint64_t reserve_value;
    bool reserve_valid;

    inline uint64_t get_cr() const {
        uint32_t v = 0;
        for (int i = 0; i < 8; ++i) v |= cr[i].get() << (28 - 4 * i);
        return v;
    }
    inline void set_cr(uint32_t v, uint32_t fxm) {
        for (int i = 0; i < 8; ++i)
            if (fxm & (0x80 >> i)) cr[i].set(v >> (28 - 4 * i));
    }
    inline void update_cr1() { cr[1].set(fpscr >> 28); }
    inline void set_fpscr(uint32_t v, uint32_t fm) {
        for (int i = 0; i < 8; ++i)
            if (fm & (0x80 >> i)) {
                uint32_t m = 0xFu << (28 - 4 * i);
                fpscr = (fpscr & ~m) | (v & m);
            }
    }
    inline void set_fpscr_field(int field, uint32_t imm) {
        uint32_t sh = 28 - 4 * field;
        fpscr = (fpscr & ~(0xFu << sh)) | ((imm & 0xF) << sh);
    }
};

#define PPC_FUNC(name) void name(PPCContext& __restrict ctx, uint8_t* base)
#define PPC_FUNC_IMPL(name) PPC_FUNC(name)
#define PPC_WEAK

typedef void (*PPCFunc)(PPCContext& __restrict ctx, uint8_t* base);
struct PPCFuncMapping {
    uint32_t guest;
    PPCFunc host;
};
extern const PPCFuncMapping g_ppc_func_table[];

// ---- runtime services (runtime/ppc_runtime.cpp)
void ppc_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target);
void ppc_syscall(PPCContext& ctx, uint8_t* base, uint32_t addr);
[[noreturn]] void ppc_trap(PPCContext& ctx, uint32_t addr);
[[noreturn]] void ppc_unimplemented(PPCContext& ctx, uint32_t addr, const char* mn, uint32_t word);
[[noreturn]] void ppc_unreachable(PPCContext& ctx, uint32_t addr);
void ppc_hle_unimplemented(const char* lib, const char* name, uint32_t nid, PPCContext& ctx);
uint64_t ppc_mftb();

// ---- memory
#define PPC_ADDR(ea) (base + (uint32_t)(ea))
static inline uint8_t ppc_ld8(const uint8_t* p) { return *p; }
static inline uint16_t ppc_ld16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return __builtin_bswap16(v); }
static inline uint32_t ppc_ld32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint64_t ppc_ld64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline void ppc_st16(uint8_t* p, uint16_t v) { v = __builtin_bswap16(v); memcpy(p, &v, 2); }
static inline void ppc_st32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static inline void ppc_st64(uint8_t* p, uint64_t v) { v = __builtin_bswap64(v); memcpy(p, &v, 8); }
static inline float ppc_ldf32(const uint8_t* p) { uint32_t u = ppc_ld32(p); float f; memcpy(&f, &u, 4); return f; }
static inline void ppc_stf32(uint8_t* p, float f) { uint32_t u; memcpy(&u, &f, 4); ppc_st32(p, u); }

#define PPC_LOAD_U8(ea) ((uint64_t)ppc_ld8(PPC_ADDR(ea)))
#define PPC_LOAD_U16(ea) ((uint64_t)ppc_ld16(PPC_ADDR(ea)))
#define PPC_LOAD_U32(ea) ((uint64_t)ppc_ld32(PPC_ADDR(ea)))
#define PPC_LOAD_U64(ea) (ppc_ld64(PPC_ADDR(ea)))
#define PPC_LOAD_F32(ea) (ppc_ldf32(PPC_ADDR(ea)))
#define PPC_LOAD_U16_LE(ea) ((uint64_t)__builtin_bswap16((uint16_t)ppc_ld16(PPC_ADDR(ea))))
#define PPC_LOAD_U32_LE(ea) ((uint64_t)__builtin_bswap32(ppc_ld32(PPC_ADDR(ea))))
#define PPC_LOAD_U64_LE(ea) (__builtin_bswap64(ppc_ld64(PPC_ADDR(ea))))
#define PPC_STORE_U8(ea, v) (*PPC_ADDR(ea) = (uint8_t)(v))
#define PPC_STORE_U16(ea, v) ppc_st16(PPC_ADDR(ea), (uint16_t)(v))
#define PPC_STORE_U32(ea, v) ppc_st32(PPC_ADDR(ea), (uint32_t)(v))
#define PPC_STORE_U64(ea, v) ppc_st64(PPC_ADDR(ea), (uint64_t)(v))
#define PPC_STORE_F32(ea, v) ppc_stf32(PPC_ADDR(ea), (v))
#define PPC_STORE_U16_LE(ea, v) ppc_st16(PPC_ADDR(ea), __builtin_bswap16((uint16_t)(v)))
#define PPC_STORE_U32_LE(ea, v) ppc_st32(PPC_ADDR(ea), __builtin_bswap32((uint32_t)(v)))
#define PPC_STORE_U64_LE(ea, v) ppc_st64(PPC_ADDR(ea), __builtin_bswap64((uint64_t)(v)))
#define PPC_DCBZ(ea) memset(base + ((uint32_t)(ea) & ~127u), 0, 128)  // Cell cache line = 128
#define PPC_FENCE() std::atomic_thread_fence(std::memory_order_seq_cst)

// ---- load-reserve / store-conditional (emulated with CAS on the reserved value)
static inline uint64_t ppc_lwarx(PPCContext& ctx, uint8_t* base, uint64_t ea) {
    ctx.reserve_addr = (uint32_t)ea;
    ctx.reserve_value = ppc_ld32(PPC_ADDR(ea));
    ctx.reserve_valid = true;
    return ctx.reserve_value;
}
static inline uint64_t ppc_ldarx(PPCContext& ctx, uint8_t* base, uint64_t ea) {
    ctx.reserve_addr = (uint32_t)ea;
    ctx.reserve_value = ppc_ld64(PPC_ADDR(ea));
    ctx.reserve_valid = true;
    return ctx.reserve_value;
}
static inline bool ppc_stwcx(PPCContext& ctx, uint8_t* base, uint64_t ea, uint64_t v) {
    if (!ctx.reserve_valid || ctx.reserve_addr != (uint32_t)ea) return false;
    ctx.reserve_valid = false;
    uint32_t expected = __builtin_bswap32((uint32_t)ctx.reserve_value);
    uint32_t desired = __builtin_bswap32((uint32_t)v);
    return __atomic_compare_exchange_n((uint32_t*)PPC_ADDR(ea), &expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline bool ppc_stdcx(PPCContext& ctx, uint8_t* base, uint64_t ea, uint64_t v) {
    if (!ctx.reserve_valid || ctx.reserve_addr != (uint32_t)ea) return false;
    ctx.reserve_valid = false;
    uint64_t expected = __builtin_bswap64(ctx.reserve_value);
    uint64_t desired = __builtin_bswap64(v);
    return __atomic_compare_exchange_n((uint64_t*)PPC_ADDR(ea), &expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#define PPC_LWARX(ea) ppc_lwarx(ctx, base, (ea))
#define PPC_LDARX(ea) ppc_ldarx(ctx, base, (ea))
#define PPC_STWCX(ea, v) ppc_stwcx(ctx, base, (ea), (v))
#define PPC_STDCX(ea, v) ppc_stdcx(ctx, base, (ea), (v))

// ---- integer helpers
static inline uint32_t ppc_rotl32(uint32_t x, uint32_t n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
static inline uint64_t ppc_rotl64(uint64_t x, uint64_t n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
static inline uint64_t ppc_rotl32_dup(uint64_t x, uint64_t n) {
    uint64_t r = ppc_rotl32((uint32_t)x, (uint32_t)n);
    return r | (r << 32);
}
#define PPC_ROTL32_DUP(x, n) ppc_rotl32_dup((x), (n))
#define PPC_ROTL64(x, n) ppc_rotl64((x), (n))
#define PPC_MULH_S64(a, b) ((int64_t)(((__int128)(int64_t)(a) * (__int128)(int64_t)(b)) >> 64))
#define PPC_MULH_U64(a, b) ((uint64_t)(((unsigned __int128)(a) * (unsigned __int128)(b)) >> 64))
static inline uint64_t ppc_divw(uint64_t a, uint64_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0 || (x == INT32_MIN && y == -1)) return 0;
    return (uint64_t)(uint32_t)(x / y);
}
static inline uint64_t ppc_divd(uint64_t a, uint64_t b) {
    int64_t x = (int64_t)a, y = (int64_t)b;
    if (y == 0 || (x == INT64_MIN && y == -1)) return 0;
    return (uint64_t)(x / y);
}
#define PPC_DIVW(a, b) ppc_divw((a), (b))
#define PPC_DIVD(a, b) ppc_divd((a), (b))
static inline uint8_t ppc_carry3(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t s = a + b;
    return (s < a) | ((s + c) < s);
}
#define PPC_CARRY3(a, b, c) ppc_carry3((a), (b), (c))
#define PPC_CNTLZ32(x) ((x) ? (uint64_t)__builtin_clz((uint32_t)(x)) : 32ull)
#define PPC_CNTLZ64(x) ((x) ? (uint64_t)__builtin_clzll((uint64_t)(x)) : 64ull)
#define PPC_MFTB() ppc_mftb()

// ---- floating point helpers
#define PPC_FMA(a, b, c) std::fma((a), (b), (c))
#define PPC_SQRT(x) std::sqrt(x)
static inline int32_t ppc_f2i32(double d) {
    if (std::isnan(d)) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}
static inline int64_t ppc_f2i64(double d) {
    if (std::isnan(d)) return INT64_MIN;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}
#define PPC_F2I32(d) ppc_f2i32(d)
#define PPC_F2I64(d) ppc_f2i64(d)
#define PPC_F2I32_RN(d) ppc_f2i32(std::nearbyint(d))
#define PPC_F2I64_RN(d) ppc_f2i64(std::nearbyint(d))

// ---- control flow / system
#define PPC_CALL_INDIRECT(t) ppc_call_indirect(ctx, base, (uint32_t)(t))
#define PPC_SYSCALL(addr) ppc_syscall(ctx, base, (addr))
#define PPC_TRAP(addr) ppc_trap(ctx, (addr))
#define PPC_UNIMPLEMENTED(addr, mn, w) ppc_unimplemented(ctx, (addr), (mn), (w))
#define PPC_UNREACHABLE(addr) ppc_unreachable(ctx, (addr))
#define PPC_FALLTHROUGH(addr) ppc_call_indirect(ctx, base, (addr))

#include "ppc_vmx.h"
