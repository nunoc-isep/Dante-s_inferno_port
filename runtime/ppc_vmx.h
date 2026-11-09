// VMX / AltiVec helpers for the recompiled PPU code.
//
// Storage convention: a guest vector is kept as its 16 big-endian bytes in
// fully reversed order, so host u8[15-i] is guest byte i, host u32[3-k] is
// guest word k (as a host-endian value), host f32[3-k] is guest float k.
// All width views stay consistent; lane-wise ops ignore the mapping and only
// order-sensitive ops (splat, merge, pack, perm, shifts by octet, sums) use
// the BE accessors below.
//
// Float behaviour: Cell VXU runs in non-Java mode (VSCR[NJ]=1), so denormal
// inputs and results are flushed to zero.
#pragma once

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#define VMX_T(x) std::remove_reference_t<decltype(x)>

// ---- big-endian element accessors
#define VB(v, i) ((v).u8[15 - (i)])
#define VH(v, i) ((v).u16[7 - (i)])
#define VW(v, i) ((v).u32[3 - (i)])
#define VF(v, i) ((v).f32[3 - (i)])
#define VSB(v, i) (*(int8_t*)&(v).u8[15 - (i)])
#define VSH(v, i) (*(int16_t*)&(v).u16[7 - (i)])
#define VSW(v, i) (*(int32_t*)&(v).u32[3 - (i)])

// ---- loads / stores
static inline PPCVR vmx_load(const uint8_t* base, uint64_t ea) {
    const uint8_t* p = base + ((uint32_t)ea & ~15u);
    PPCVR v;
    for (int i = 0; i < 16; ++i) v.u8[15 - i] = p[i];
    return v;
}
static inline void vmx_store(uint8_t* base, uint64_t ea, const PPCVR& v) {
    uint8_t* p = base + ((uint32_t)ea & ~15u);
    for (int i = 0; i < 16; ++i) p[i] = v.u8[15 - i];
}
// Cell lvlx: bytes from ea up to the 16-byte boundary go to the left of the vector.
static inline PPCVR vmx_lvlx(const uint8_t* base, uint64_t ea) {
    uint32_t a = (uint32_t)ea, sh = a & 15;
    PPCVR al = vmx_load(base, a), v{};
    for (int i = 0; i < 16 - (int)sh; ++i) VB(v, i) = VB(al, i + sh);
    return v;
}
// Cell lvrx: bytes from the aligned base up to ea go to the right of the vector.
static inline PPCVR vmx_lvrx(const uint8_t* base, uint64_t ea) {
    uint32_t a = (uint32_t)ea, sh = a & 15;
    PPCVR al = vmx_load(base, a), v{};
    for (int i = 0; i < (int)sh; ++i) VB(v, 16 - sh + i) = VB(al, i);
    return v;
}
static inline void vmx_stvlx(uint8_t* base, uint64_t ea, const PPCVR& v) {
    uint32_t a = (uint32_t)ea, sh = a & 15;
    for (uint32_t i = 0; i < 16 - sh; ++i) base[a + i] = VB(v, i);
}
static inline void vmx_stvrx(uint8_t* base, uint64_t ea, const PPCVR& v) {
    uint32_t a = (uint32_t)ea, sh = a & 15, al = a & ~15u;
    for (uint32_t i = 0; i < sh; ++i) base[al + i] = VB(v, 16 - sh + i);
}
static inline void vmx_stvebx(uint8_t* base, uint64_t ea, const PPCVR& v) {
    uint32_t a = (uint32_t)ea;
    base[a] = VB(v, a & 15);
}
static inline void vmx_stvehx(uint8_t* base, uint64_t ea, const PPCVR& v) {
    uint32_t a = (uint32_t)ea & ~1u;
    uint16_t h = VH(v, (a & 15) >> 1);
    base[a] = h >> 8;
    base[a + 1] = h & 0xFF;
}
static inline void vmx_stvewx(uint8_t* base, uint64_t ea, const PPCVR& v) {
    uint32_t a = (uint32_t)ea & ~3u;
    uint32_t w = VW(v, (a & 15) >> 2);
    base[a] = w >> 24;
    base[a + 1] = (w >> 16) & 0xFF;
    base[a + 2] = (w >> 8) & 0xFF;
    base[a + 3] = w & 0xFF;
}
static inline PPCVR vmx_lvsl(uint64_t ea) {
    PPCVR v;
    uint32_t sh = (uint32_t)ea & 15;
    for (int i = 0; i < 16; ++i) VB(v, i) = (uint8_t)(sh + i);
    return v;
}
static inline PPCVR vmx_lvsr(uint64_t ea) {
    PPCVR v;
    uint32_t sh = (uint32_t)ea & 15;
    for (int i = 0; i < 16; ++i) VB(v, i) = (uint8_t)(16 - sh + i);
    return v;
}

// ---- float helpers
static inline float vmx_ftz(float f) {
    return std::fpclassify(f) == FP_SUBNORMAL ? std::copysign(0.0f, f) : f;
}
#define VMX_FOP2(name, expr)                                                         \
    static inline PPCVR name(const PPCVR& a, const PPCVR& b) {                       \
        PPCVR r;                                                                     \
        for (int i = 0; i < 4; ++i) {                                                \
            float x = vmx_ftz(a.f32[i]), y = vmx_ftz(b.f32[i]);                      \
            r.f32[i] = vmx_ftz(expr);                                                \
        }                                                                            \
        return r;                                                                    \
    }
VMX_FOP2(vmx_vaddfp, x + y)
VMX_FOP2(vmx_vsubfp, x - y)
VMX_FOP2(vmx_vmaxfp, (std::isnan(x) || std::isnan(y)) ? (std::isnan(x) ? x : y) : std::max(x, y))
VMX_FOP2(vmx_vminfp, (std::isnan(x) || std::isnan(y)) ? (std::isnan(x) ? x : y) : std::min(x, y))

// vmaddfp vD = vA*vC + vB (fused); vnmsubfp vD = -(vA*vC - vB)
static inline PPCVR vmx_vmaddfp(const PPCVR& a, const PPCVR& c, const PPCVR& b) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) r.f32[i] = vmx_ftz(std::fma(vmx_ftz(a.f32[i]), vmx_ftz(c.f32[i]), vmx_ftz(b.f32[i])));
    return r;
}
static inline PPCVR vmx_vnmsubfp(const PPCVR& a, const PPCVR& c, const PPCVR& b) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) r.f32[i] = vmx_ftz(-std::fma(vmx_ftz(a.f32[i]), vmx_ftz(c.f32[i]), -vmx_ftz(b.f32[i])));
    return r;
}
#define VMX_FOP1(name, expr)                                  \
    static inline PPCVR name(const PPCVR& b) {                \
        PPCVR r;                                              \
        for (int i = 0; i < 4; ++i) {                         \
            float x = vmx_ftz(b.f32[i]);                      \
            r.f32[i] = vmx_ftz(expr);                         \
        }                                                     \
        return r;                                             \
    }
VMX_FOP1(vmx_vrefp, 1.0f / x)
VMX_FOP1(vmx_vrsqrtefp, 1.0f / std::sqrt(x))
VMX_FOP1(vmx_vexptefp, std::exp2(x))
VMX_FOP1(vmx_vlogefp, std::log2(x))
VMX_FOP1(vmx_vrfin, std::nearbyint(x))
VMX_FOP1(vmx_vrfiz, std::trunc(x))
VMX_FOP1(vmx_vrfip, std::ceil(x))
VMX_FOP1(vmx_vrfim, std::floor(x))

static inline PPCVR vmx_vcfsx(const PPCVR& b, int uimm) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) r.f32[i] = std::ldexp((float)(int32_t)b.u32[i], -uimm);
    return r;
}
static inline PPCVR vmx_vcfux(const PPCVR& b, int uimm) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) r.f32[i] = std::ldexp((float)b.u32[i], -uimm);
    return r;
}
static inline PPCVR vmx_vctsxs(PPCContext& ctx, const PPCVR& b, int uimm) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) {
        double x = std::ldexp((double)b.f32[i], uimm);
        int32_t v;
        if (std::isnan(x)) { v = 0; ctx.vscr |= 1; }
        else if (x >= 2147483647.0) { v = INT32_MAX; ctx.vscr |= 1; }
        else if (x < -2147483648.0) { v = INT32_MIN; ctx.vscr |= 1; }
        else v = (int32_t)std::trunc(x);
        r.u32[i] = (uint32_t)v;
    }
    return r;
}
static inline PPCVR vmx_vctuxs(PPCContext& ctx, const PPCVR& b, int uimm) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) {
        double x = std::ldexp((double)b.f32[i], uimm);
        uint32_t v;
        if (std::isnan(x) || x < 0) { v = 0; if (x < 0 || std::isnan(x)) ctx.vscr |= 1; }
        else if (x >= 4294967295.0) { v = UINT32_MAX; ctx.vscr |= 1; }
        else v = (uint32_t)std::trunc(x);
        r.u32[i] = v;
    }
    return r;
}

// ---- compares (result mask; optional CR6 update: lt = all true, eq = none true)
static inline void vmx_set_cr6(PPCContext& ctx, const PPCVR& r) {
    bool all = (r.u64[0] & r.u64[1]) == ~0ull, none = (r.u64[0] | r.u64[1]) == 0;
    ctx.cr[6].lt = all;
    ctx.cr[6].gt = 0;
    ctx.cr[6].eq = none;
    ctx.cr[6].so = 0;
}
#define VMX_CMP(name, field, n, T, expr)                                     \
    static inline PPCVR name(const PPCVR& a, const PPCVR& b) {               \
        PPCVR r;                                                             \
        for (int i = 0; i < n; ++i) {                                        \
            T x = (T)a.field[i], y = (T)b.field[i];                          \
            r.field[i] = (expr) ? (VMX_T(r.field[0]))~0u : 0;             \
        }                                                                    \
        return r;                                                            \
    }
VMX_CMP(vmx_vcmpequb, u8, 16, uint8_t, x == y)
VMX_CMP(vmx_vcmpequh, u16, 8, uint16_t, x == y)
VMX_CMP(vmx_vcmpequw, u32, 4, uint32_t, x == y)
VMX_CMP(vmx_vcmpgtub, u8, 16, uint8_t, x > y)
VMX_CMP(vmx_vcmpgtuh, u16, 8, uint16_t, x > y)
VMX_CMP(vmx_vcmpgtuw, u32, 4, uint32_t, x > y)
VMX_CMP(vmx_vcmpgtsb, u8, 16, int8_t, x > y)
VMX_CMP(vmx_vcmpgtsh, u16, 8, int16_t, x > y)
VMX_CMP(vmx_vcmpgtsw, u32, 4, int32_t, x > y)
static inline PPCVR vmx_fcmp(const PPCVR& a, const PPCVR& b, int kind) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) {
        float x = a.f32[i], y = b.f32[i];
        bool t = kind == 0 ? x == y : kind == 1 ? x >= y : x > y;
        r.u32[i] = t ? ~0u : 0;
    }
    return r;
}
#define vmx_vcmpeqfp(a, b) vmx_fcmp((a), (b), 0)
#define vmx_vcmpgefp(a, b) vmx_fcmp((a), (b), 1)
#define vmx_vcmpgtfp(a, b) vmx_fcmp((a), (b), 2)
static inline PPCVR vmx_vcmpbfp(const PPCVR& a, const PPCVR& b) {
    PPCVR r;
    for (int i = 0; i < 4; ++i) {
        float x = a.f32[i], y = b.f32[i];
        uint32_t v = 0;
        if (!(x <= y)) v |= 0x80000000u;
        if (!(x >= -y)) v |= 0x40000000u;
        r.u32[i] = v;
    }
    return r;
}
static inline void vmx_set_cr6_bounds(PPCContext& ctx, const PPCVR& r) {
    ctx.cr[6].lt = 0;
    ctx.cr[6].gt = 0;
    ctx.cr[6].eq = (r.u64[0] | r.u64[1]) == 0;
    ctx.cr[6].so = 0;
}

// ---- integer lane ops
template <typename T> static inline T vmx_sat(int64_t v) {
    if (v > (int64_t)std::numeric_limits<T>::max()) return std::numeric_limits<T>::max();
    if (v < (int64_t)std::numeric_limits<T>::min()) return std::numeric_limits<T>::min();
    return (T)v;
}
#define VMX_IOP(name, field, n, T, expr)                                     \
    static inline PPCVR name(const PPCVR& a, const PPCVR& b) {               \
        PPCVR r;                                                             \
        for (int i = 0; i < n; ++i) {                                        \
            T x = (T)a.field[i], y = (T)b.field[i];                          \
            r.field[i] = (VMX_T(r.field[0]))(expr);                       \
        }                                                                    \
        return r;                                                            \
    }
VMX_IOP(vmx_vaddubm, u8, 16, uint8_t, x + y)
VMX_IOP(vmx_vadduhm, u16, 8, uint16_t, x + y)
VMX_IOP(vmx_vadduwm, u32, 4, uint32_t, x + y)
VMX_IOP(vmx_vsububm, u8, 16, uint8_t, x - y)
VMX_IOP(vmx_vsubuhm, u16, 8, uint16_t, x - y)
VMX_IOP(vmx_vsubuwm, u32, 4, uint32_t, x - y)
VMX_IOP(vmx_vaddubs, u8, 16, uint8_t, vmx_sat<uint8_t>((int64_t)x + y))
VMX_IOP(vmx_vadduhs, u16, 8, uint16_t, vmx_sat<uint16_t>((int64_t)x + y))
VMX_IOP(vmx_vadduws, u32, 4, uint32_t, vmx_sat<uint32_t>((int64_t)x + y))
VMX_IOP(vmx_vaddsbs, u8, 16, int8_t, vmx_sat<int8_t>((int64_t)x + y))
VMX_IOP(vmx_vaddshs, u16, 8, int16_t, vmx_sat<int16_t>((int64_t)x + y))
VMX_IOP(vmx_vaddsws, u32, 4, int32_t, vmx_sat<int32_t>((int64_t)x + y))
VMX_IOP(vmx_vsububs, u8, 16, uint8_t, vmx_sat<uint8_t>((int64_t)x - y))
VMX_IOP(vmx_vsubuhs, u16, 8, uint16_t, vmx_sat<uint16_t>((int64_t)x - y))
VMX_IOP(vmx_vsubuws, u32, 4, uint32_t, vmx_sat<uint32_t>((int64_t)x - y))
VMX_IOP(vmx_vsubsbs, u8, 16, int8_t, vmx_sat<int8_t>((int64_t)x - y))
VMX_IOP(vmx_vsubshs, u16, 8, int16_t, vmx_sat<int16_t>((int64_t)x - y))
VMX_IOP(vmx_vsubsws, u32, 4, int32_t, vmx_sat<int32_t>((int64_t)x - y))
VMX_IOP(vmx_vaddcuw, u32, 4, uint32_t, ((uint64_t)x + y) >> 32)
VMX_IOP(vmx_vsubcuw, u32, 4, uint32_t, x >= y ? 1 : 0)
VMX_IOP(vmx_vmaxub, u8, 16, uint8_t, std::max(x, y))
VMX_IOP(vmx_vmaxuh, u16, 8, uint16_t, std::max(x, y))
VMX_IOP(vmx_vmaxuw, u32, 4, uint32_t, std::max(x, y))
VMX_IOP(vmx_vmaxsb, u8, 16, int8_t, std::max(x, y))
VMX_IOP(vmx_vmaxsh, u16, 8, int16_t, std::max(x, y))
VMX_IOP(vmx_vmaxsw, u32, 4, int32_t, std::max(x, y))
VMX_IOP(vmx_vminub, u8, 16, uint8_t, std::min(x, y))
VMX_IOP(vmx_vminuh, u16, 8, uint16_t, std::min(x, y))
VMX_IOP(vmx_vminuw, u32, 4, uint32_t, std::min(x, y))
VMX_IOP(vmx_vminsb, u8, 16, int8_t, std::min(x, y))
VMX_IOP(vmx_vminsh, u16, 8, int16_t, std::min(x, y))
VMX_IOP(vmx_vminsw, u32, 4, int32_t, std::min(x, y))
VMX_IOP(vmx_vavgub, u8, 16, uint8_t, ((uint32_t)x + y + 1) >> 1)
VMX_IOP(vmx_vavguh, u16, 8, uint16_t, ((uint32_t)x + y + 1) >> 1)
VMX_IOP(vmx_vavguw, u32, 4, uint32_t, ((uint64_t)x + y + 1) >> 1)
VMX_IOP(vmx_vavgsb, u8, 16, int8_t, ((int32_t)x + y + 1) >> 1)
VMX_IOP(vmx_vavgsh, u16, 8, int16_t, ((int32_t)x + y + 1) >> 1)
VMX_IOP(vmx_vavgsw, u32, 4, int32_t, ((int64_t)x + y + 1) >> 1)
VMX_IOP(vmx_vslb, u8, 16, uint8_t, (uint8_t)(x << (y & 7)))
VMX_IOP(vmx_vslh, u16, 8, uint16_t, (uint16_t)(x << (y & 15)))
VMX_IOP(vmx_vslw, u32, 4, uint32_t, x << (y & 31))
VMX_IOP(vmx_vsrb, u8, 16, uint8_t, x >> (y & 7))
VMX_IOP(vmx_vsrh, u16, 8, uint16_t, x >> (y & 15))
VMX_IOP(vmx_vsrw, u32, 4, uint32_t, x >> (y & 31))
VMX_IOP(vmx_vsrab, u8, 16, int8_t, x >> ((uint8_t)y & 7))
VMX_IOP(vmx_vsrah, u16, 8, int16_t, x >> ((uint16_t)y & 15))
VMX_IOP(vmx_vsraw, u32, 4, int32_t, x >> ((uint32_t)y & 31))
VMX_IOP(vmx_vrlb, u8, 16, uint8_t, (uint8_t)((x << (y & 7)) | (x >> ((8 - (y & 7)) & 7))))
VMX_IOP(vmx_vrlh, u16, 8, uint16_t, (uint16_t)((x << (y & 15)) | (x >> ((16 - (y & 15)) & 15))))
VMX_IOP(vmx_vrlw, u32, 4, uint32_t, ppc_rotl32(x, y & 31))

// ---- logical
static inline PPCVR vmx_vand(const PPCVR& a, const PPCVR& b) { PPCVR r; r.u64[0] = a.u64[0] & b.u64[0]; r.u64[1] = a.u64[1] & b.u64[1]; return r; }
static inline PPCVR vmx_vandc(const PPCVR& a, const PPCVR& b) { PPCVR r; r.u64[0] = a.u64[0] & ~b.u64[0]; r.u64[1] = a.u64[1] & ~b.u64[1]; return r; }
static inline PPCVR vmx_vor(const PPCVR& a, const PPCVR& b) { PPCVR r; r.u64[0] = a.u64[0] | b.u64[0]; r.u64[1] = a.u64[1] | b.u64[1]; return r; }
static inline PPCVR vmx_vnor(const PPCVR& a, const PPCVR& b) { PPCVR r; r.u64[0] = ~(a.u64[0] | b.u64[0]); r.u64[1] = ~(a.u64[1] | b.u64[1]); return r; }
static inline PPCVR vmx_vxor(const PPCVR& a, const PPCVR& b) { PPCVR r; r.u64[0] = a.u64[0] ^ b.u64[0]; r.u64[1] = a.u64[1] ^ b.u64[1]; return r; }
static inline PPCVR vmx_vsel(const PPCVR& a, const PPCVR& b, const PPCVR& c) {
    PPCVR r;
    for (int i = 0; i < 2; ++i) r.u64[i] = (a.u64[i] & ~c.u64[i]) | (b.u64[i] & c.u64[i]);
    return r;
}

// ---- permutes / splats / merges
static inline PPCVR vmx_vperm(const PPCVR& a, const PPCVR& b, const PPCVR& c) {
    PPCVR r;
    for (int i = 0; i < 16; ++i) {
        int idx = VB(c, i) & 31;
        VB(r, i) = idx < 16 ? VB(a, idx) : VB(b, idx - 16);
    }
    return r;
}
static inline PPCVR vmx_vsldoi(const PPCVR& a, const PPCVR& b, int sh) {
    PPCVR r;
    for (int i = 0; i < 16; ++i) {
        int k = i + sh;
        VB(r, i) = k < 16 ? VB(a, k) : VB(b, k - 16);
    }
    return r;
}
static inline PPCVR vmx_vspltb(const PPCVR& b, int i) { PPCVR r; uint8_t x = VB(b, i & 15); memset(r.u8, x, 16); return r; }
static inline PPCVR vmx_vsplth(const PPCVR& b, int i) { PPCVR r; uint16_t x = VH(b, i & 7); for (auto& e : r.u16) e = x; return r; }
static inline PPCVR vmx_vspltw(const PPCVR& b, int i) { PPCVR r; uint32_t x = VW(b, i & 3); for (auto& e : r.u32) e = x; return r; }
static inline PPCVR vmx_vspltisb(int s) { PPCVR r; memset(r.u8, (uint8_t)(int8_t)s, 16); return r; }
static inline PPCVR vmx_vspltish(int s) { PPCVR r; for (auto& e : r.u16) e = (uint16_t)(int16_t)s; return r; }
static inline PPCVR vmx_vspltisw(int s) { PPCVR r; for (auto& e : r.u32) e = (uint32_t)(int32_t)s; return r; }
#define VMX_MERGE(name, ACC, n, off)                                         \
    static inline PPCVR name(const PPCVR& a, const PPCVR& b) {               \
        PPCVR r;                                                             \
        for (int i = 0; i < n / 2; ++i) {                                    \
            ACC(r, 2 * i) = ACC(a, i + off);                                 \
            ACC(r, 2 * i + 1) = ACC(b, i + off);                             \
        }                                                                    \
        return r;                                                            \
    }
VMX_MERGE(vmx_vmrghb, VB, 16, 0)
VMX_MERGE(vmx_vmrglb, VB, 16, 8)
VMX_MERGE(vmx_vmrghh, VH, 8, 0)
VMX_MERGE(vmx_vmrglh, VH, 8, 4)
VMX_MERGE(vmx_vmrghw, VW, 4, 0)
VMX_MERGE(vmx_vmrglw, VW, 4, 2)

// whole-vector shifts
static inline PPCVR vmx_vslo(const PPCVR& a, const PPCVR& b) {
    int sh = (VB(b, 15) >> 3) & 15;
    PPCVR r;
    for (int i = 0; i < 16; ++i) VB(r, i) = i + sh < 16 ? VB(a, i + sh) : 0;
    return r;
}
static inline PPCVR vmx_vsro(const PPCVR& a, const PPCVR& b) {
    int sh = (VB(b, 15) >> 3) & 15;
    PPCVR r;
    for (int i = 0; i < 16; ++i) VB(r, i) = i >= sh ? VB(a, i - sh) : 0;
    return r;
}
static inline PPCVR vmx_vsl(const PPCVR& a, const PPCVR& b) {
    int sh = VB(b, 15) & 7;
    PPCVR r;
    for (int i = 0; i < 16; ++i) {
        uint8_t next = i < 15 ? VB(a, i + 1) : 0;
        VB(r, i) = (uint8_t)((VB(a, i) << sh) | (sh ? next >> (8 - sh) : 0));
    }
    return r;
}
static inline PPCVR vmx_vsr(const PPCVR& a, const PPCVR& b) {
    int sh = VB(b, 15) & 7;
    PPCVR r;
    for (int i = 0; i < 16; ++i) {
        uint8_t prev = i > 0 ? VB(a, i - 1) : 0;
        VB(r, i) = (uint8_t)((VB(a, i) >> sh) | (sh ? prev << (8 - sh) : 0));
    }
    return r;
}

// ---- pack / unpack (BE element order: vA fills the high half)
#define VMX_PACK(name, SRC, DST, n, T, conv)                                 \
    static inline PPCVR name(PPCContext& ctx, const PPCVR& a, const PPCVR& b) { \
        PPCVR r;                                                             \
        (void)ctx;                                                           \
        for (int i = 0; i < n; ++i) {                                        \
            T x = (T)SRC(a, i), y = (T)SRC(b, i);                            \
            DST(r, i) = conv(x);                                             \
            DST(r, i + n) = conv(y);                                         \
        }                                                                    \
        return r;                                                            \
    }
#define VMX_TRUNC8(x) (uint8_t)(x)
#define VMX_TRUNC16(x) (uint16_t)(x)
#define VMX_SAT(T, x) vmx_sat_flag<T>(ctx, (int64_t)(x))
template <typename T> static inline T vmx_sat_flag(PPCContext& ctx, int64_t v) {
    T s = vmx_sat<T>(v);
    if ((int64_t)s != v) ctx.vscr |= 1;
    return s;
}
VMX_PACK(vmx_vpkuhum, VH, VB, 8, uint16_t, VMX_TRUNC8)
VMX_PACK(vmx_vpkuwum, VW, VH, 4, uint32_t, VMX_TRUNC16)
#define VMX_S_U8(x) (uint8_t)VMX_SAT(uint8_t, x)
#define VMX_S_S8(x) (uint8_t)VMX_SAT(int8_t, x)
#define VMX_S_U16(x) (uint16_t)VMX_SAT(uint16_t, x)
#define VMX_S_S16(x) (uint16_t)VMX_SAT(int16_t, x)
VMX_PACK(vmx_vpkuhus, VH, VB, 8, uint16_t, VMX_S_U8)
VMX_PACK(vmx_vpkshus, VSH, VB, 8, int16_t, VMX_S_U8)
VMX_PACK(vmx_vpkshss, VSH, VB, 8, int16_t, VMX_S_S8)
VMX_PACK(vmx_vpkuwus, VW, VH, 4, uint32_t, VMX_S_U16)
VMX_PACK(vmx_vpkswus, VSW, VH, 4, int32_t, VMX_S_U16)
VMX_PACK(vmx_vpkswss, VSW, VH, 4, int32_t, VMX_S_S16)
static inline PPCVR vmx_vupkhsb(const PPCVR& b) { PPCVR r; for (int i = 0; i < 8; ++i) VH(r, i) = (uint16_t)(int16_t)VSB(b, i); return r; }
static inline PPCVR vmx_vupklsb(const PPCVR& b) { PPCVR r; for (int i = 0; i < 8; ++i) VH(r, i) = (uint16_t)(int16_t)VSB(b, i + 8); return r; }
static inline PPCVR vmx_vupkhsh(const PPCVR& b) { PPCVR r; for (int i = 0; i < 4; ++i) VW(r, i) = (uint32_t)(int32_t)VSH(b, i); return r; }
static inline PPCVR vmx_vupklsh(const PPCVR& b) { PPCVR r; for (int i = 0; i < 4; ++i) VW(r, i) = (uint32_t)(int32_t)VSH(b, i + 4); return r; }

// ---- multiplies (even = BE element 0,2,..; odd = 1,3,..)
#define VMX_MULEO(name, SRC, DST, n, T, odd)                                 \
    static inline PPCVR name(const PPCVR& a, const PPCVR& b) {               \
        PPCVR r;                                                             \
        for (int i = 0; i < n; ++i)                                          \
            DST(r, i) = (VMX_T(DST(r, 0)))((T)SRC(a, 2 * i + odd) * (T)SRC(b, 2 * i + odd)); \
        return r;                                                            \
    }
VMX_MULEO(vmx_vmuleub, VB, VH, 8, uint32_t, 0)
VMX_MULEO(vmx_vmuloub, VB, VH, 8, uint32_t, 1)
VMX_MULEO(vmx_vmulesb, VSB, VH, 8, int32_t, 0)
VMX_MULEO(vmx_vmulosb, VSB, VH, 8, int32_t, 1)
VMX_MULEO(vmx_vmuleuh, VH, VW, 4, uint32_t, 0)
VMX_MULEO(vmx_vmulouh, VH, VW, 4, uint32_t, 1)
VMX_MULEO(vmx_vmulesh, VSH, VW, 4, int32_t, 0)
VMX_MULEO(vmx_vmulosh, VSH, VW, 4, int32_t, 1)

// ---- sums
static inline PPCVR vmx_vsumsws(PPCContext& ctx, const PPCVR& a, const PPCVR& b) {
    int64_t s = (int64_t)VSW(b, 3);
    for (int i = 0; i < 4; ++i) s += VSW(a, i);
    PPCVR r{};
    VW(r, 3) = (uint32_t)vmx_sat_flag<int32_t>(ctx, s);
    return r;
}
static inline PPCVR vmx_vsum2sws(PPCContext& ctx, const PPCVR& a, const PPCVR& b) {
    PPCVR r{};
    for (int k = 0; k < 2; ++k) {
        int64_t s = (int64_t)VSW(b, 2 * k + 1) + VSW(a, 2 * k) + VSW(a, 2 * k + 1);
        VW(r, 2 * k + 1) = (uint32_t)vmx_sat_flag<int32_t>(ctx, s);
    }
    return r;
}
static inline PPCVR vmx_vsum4ubs(PPCContext& ctx, const PPCVR& a, const PPCVR& b) {
    PPCVR r;
    for (int k = 0; k < 4; ++k) {
        int64_t s = (int64_t)VW(b, k);
        for (int j = 0; j < 4; ++j) s += VB(a, 4 * k + j);
        VW(r, k) = vmx_sat_flag<uint32_t>(ctx, s);
    }
    return r;
}
static inline PPCVR vmx_vsum4sbs(PPCContext& ctx, const PPCVR& a, const PPCVR& b) {
    PPCVR r;
    for (int k = 0; k < 4; ++k) {
        int64_t s = (int64_t)VSW(b, k);
        for (int j = 0; j < 4; ++j) s += VSB(a, 4 * k + j);
        VW(r, k) = (uint32_t)vmx_sat_flag<int32_t>(ctx, s);
    }
    return r;
}
static inline PPCVR vmx_vsum4shs(PPCContext& ctx, const PPCVR& a, const PPCVR& b) {
    PPCVR r;
    for (int k = 0; k < 4; ++k) {
        int64_t s = (int64_t)VSW(b, k) + VSH(a, 2 * k) + VSH(a, 2 * k + 1);
        VW(r, k) = (uint32_t)vmx_sat_flag<int32_t>(ctx, s);
    }
    return r;
}
// multiply-sum (VA form, vD = sum(vA*vB) + vC)
static inline PPCVR vmx_vmsumubm(const PPCVR& a, const PPCVR& b, const PPCVR& c) {
    PPCVR r;
    for (int k = 0; k < 4; ++k) {
        uint32_t s = VW(c, k);
        for (int j = 0; j < 4; ++j) s += (uint32_t)VB(a, 4 * k + j) * VB(b, 4 * k + j);
        VW(r, k) = s;
    }
    return r;
}
static inline PPCVR vmx_vmsumshm(const PPCVR& a, const PPCVR& b, const PPCVR& c) {
    PPCVR r;
    for (int k = 0; k < 4; ++k) {
        int32_t s = VSW(c, k);
        for (int j = 0; j < 2; ++j) s += (int32_t)VSH(a, 2 * k + j) * VSH(b, 2 * k + j);
        VW(r, k) = (uint32_t)s;
    }
    return r;
}
static inline PPCVR vmx_vmsumuhm(const PPCVR& a, const PPCVR& b, const PPCVR& c) {
    PPCVR r;
    for (int k = 0; k < 4; ++k) {
        uint32_t s = VW(c, k);
        for (int j = 0; j < 2; ++j) s += (uint32_t)VH(a, 2 * k + j) * VH(b, 2 * k + j);
        VW(r, k) = s;
    }
    return r;
}
static inline PPCVR vmx_vmladduhm(const PPCVR& a, const PPCVR& b, const PPCVR& c) {
    PPCVR r;
    for (int i = 0; i < 8; ++i) r.u16[i] = (uint16_t)(a.u16[i] * b.u16[i] + c.u16[i]);
    return r;
}
