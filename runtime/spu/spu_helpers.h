// Helpers and macros for the per-instruction SPU implementations (spu_ops.h).
// Registers use the byte-reversed layout (see ppc_vmx.h): VW(r, 0) is the preferred
// word slot; q() gives the register as a 128-bit big-endian number.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

#include "spu.h"

typedef unsigned __int128 u128;
static inline u128 q(const PPCVR& v) { return ((u128)v.u64[1] << 64) | v.u64[0]; }
static inline void setq(PPCVR& v, u128 x) {
    v.u64[0] = (uint64_t)x;
    v.u64[1] = (uint64_t)(x >> 64);
}
static inline int32_t sx(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }
static inline void lq(SpuThread& t, PPCVR& r, uint32_t a) {
    a &= 0x3FFF0;
    uint64_t hi, lo;
    memcpy(&hi, t.ls + a, 8);
    memcpy(&lo, t.ls + a + 8, 8);
    r.u64[1] = __builtin_bswap64(hi);
    r.u64[0] = __builtin_bswap64(lo);
}
static inline void stq(SpuThread& t, const PPCVR& r, uint32_t a) {
    a &= 0x3FFF0;
    uint64_t hi = __builtin_bswap64(r.u64[1]), lo = __builtin_bswap64(r.u64[0]);
    memcpy(t.ls + a, &hi, 8);
    memcpy(t.ls + a + 8, &lo, 8);
}
static inline float ftz(float f) {  // flush denormals to zero (SPU single precision has none)
    uint32_t u;
    memcpy(&u, &f, 4);
    if ((u & 0x7F800000) == 0) u &= 0x80000000;
    memcpy(&f, &u, 4);
    return f;
}
static inline uint32_t sat_f2i(double x, bool sign) {
    if (sign) {
        if (std::isnan(x)) return 0;
        if (x >= 2147483647.0) return 0x7FFFFFFF;
        if (x <= -2147483648.0) return 0x80000000;
        return (uint32_t)(int32_t)x;
    }
    if (std::isnan(x) || x <= 0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFF;
    return (uint32_t)x;
}
// shuffle-control generation helper for cbd/chd/cwd/cdd and the x forms
static inline void insert_ctrl(PPCVR& r, uint32_t t, int size) {
    for (int i = 0; i < 16; ++i) VB(r, i) = (uint8_t)(0x10 + i);
    uint32_t pos = t & (16 - size) & 15;
    for (int i = 0; i < size; ++i) VB(r, pos + i) = (uint8_t)(i + (size == 1 ? 3 : size == 2 ? 2 : 0));
}

// saves LS + registers of a thread that stopped abnormally (spu_interp.cpp)
void spu_dump_state(SpuThread& t, uint32_t pc, const char* why);

// exec_<op> returns 0 to continue at npc, or SPU_EXIT(code): stop the thread with that code.
// `g` is the register file: t.gpr in the interpreter, a block-local copy in recompiled code (so
// the compiler can keep SPU registers in host registers; see tools/spu_recomp.py).
#define SPU_EXIT(code) ((1ull << 32) | (uint32_t)(code))
#define SPU_IMPL(name)                                                                                    \
    [[maybe_unused]] static inline __attribute__((always_inline)) uint64_t exec_##name(                  \
        SpuThread& t, PPCVR* g, uint32_t w, uint32_t pc, uint32_t& npc)
// Field decoding; with a constant `w` (recompiled code) all of this folds away.
#define SPU_PROLOGUE                                                                                       \
    const uint32_t RT = w & 0x7F, RA = (w >> 7) & 0x7F, RB = (w >> 14) & 0x7F;                             \
    const uint32_t RT4 = (w >> 21) & 0x7F, RC4 = w & 0x7F;                                                 \
    const int32_t I7 = sx((w >> 14) & 0x7F, 7), I10 = sx((w >> 14) & 0x3FF, 10), I16 = sx((w >> 7) & 0xFFFF, 16); \
    const uint32_t U16 = (w >> 7) & 0xFFFF, I8 = (w >> 14) & 0xFF;                                         \
    PPCVR& rt = g[RT];                                                                                     \
    const PPCVR a = g[RA], b = g[RB];                                                                      \
    PPCVR r;                                                                                               \
    (void)RT; (void)RA; (void)RB; (void)RT4; (void)RC4; (void)I7; (void)I10; (void)I16; (void)U16; (void)I8; \
    (void)rt; (void)a; (void)b; (void)r; (void)npc; (void)pc; (void)t
