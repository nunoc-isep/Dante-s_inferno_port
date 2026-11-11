// Generated once from spu_interp.cpp by tools/oneoff/make_spu_ops.py, then maintained by hand.
// One inline function per SPU instruction, shared by the interpreter and the SPU recompiler.
// exec_<op>(t, g, w, pc, npc): returns 0 to continue (npc = next pc), or SPU_EXIT(code) to stop.
// Registers are accessed only through `g` (see SPU_IMPL in spu_helpers.h).
#pragma once

#include "spu_helpers.h"

SPU_IMPL(selb) {
    SPU_PROLOGUE;
    const PPCVR& c = g[RC4];
    for (int i = 0; i < 2; ++i) r.u64[i] = (a.u64[i] & ~c.u64[i]) | (b.u64[i] & c.u64[i]);
    g[RT4] = r;
    return 0;
}

SPU_IMPL(shufb) {
    SPU_PROLOGUE;
    // Registers are byte-reversed: guest byte i lives at host byte 15-i, so a control byte
    // selecting guest byte k of A/B selects host byte 15-k.
    __m128i va = _mm_loadu_si128((const __m128i*)&a), vb = _mm_loadu_si128((const __m128i*)&b);
    __m128i c = _mm_loadu_si128((const __m128i*)&g[RC4]);
    // the result byte at host position j is controlled by c.u8[j] (guest index 15-j on both sides)
    __m128i idx = _mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(0x0F)), _mm_set1_epi8(0x0F));
    // host byte j of the result comes from guest index s&15 -> host index 15-(s&15) of the source
    __m128i ra = _mm_shuffle_epi8(va, idx), rb = _mm_shuffle_epi8(vb, idx);
    __m128i use_b = _mm_cmpeq_epi8(_mm_and_si128(c, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
    __m128i v = _mm_blendv_epi8(ra, rb, use_b);
    // special controls: 10xxxxxx -> 0x00, 110xxxxx -> 0xFF, 111xxxxx -> 0x80
    __m128i hi = _mm_cmplt_epi8(c, _mm_setzero_si128());                                      // >= 0x80
    __m128i ge_c0 = _mm_cmpeq_epi8(_mm_and_si128(c, _mm_set1_epi8((char)0xC0)), _mm_set1_epi8((char)0xC0));
    __m128i ge_e0 = _mm_cmpeq_epi8(_mm_and_si128(c, _mm_set1_epi8((char)0xE0)), _mm_set1_epi8((char)0xE0));
    __m128i special = _mm_blendv_epi8(_mm_blendv_epi8(_mm_setzero_si128(), _mm_set1_epi8((char)0xFF), ge_c0),
                                      _mm_set1_epi8((char)0x80), ge_e0);
    v = _mm_blendv_epi8(v, special, hi);
    _mm_storeu_si128((__m128i*)&g[RT4], v);
    return 0;
}

SPU_IMPL(mpya) {
    SPU_PROLOGUE;
    const PPCVR& c = g[RC4];
    for (int i = 0; i < 4; ++i) r.u32[i] = (uint32_t)((int16_t)a.u32[i] * (int16_t)b.u32[i]) + c.u32[i];
    g[RT4] = r;
    return 0;
}

SPU_IMPL(fma) {
    SPU_PROLOGUE;
    const PPCVR& c = g[RC4];
    SpuOp op = SPU_fma;
    for (int i = 0; i < 4; ++i) {
        float x = ftz(a.f32[i]), y = ftz(b.f32[i]), z = ftz(c.f32[i]);
        float v = op == SPU_fma ? std::fma(x, y, z) : op == SPU_fms ? std::fma(x, y, -z) : -std::fma(x, y, -z);
        r.f32[i] = ftz(v);
    }
    g[RT4] = r;
    return 0;
}

SPU_IMPL(fms) {
    SPU_PROLOGUE;
    const PPCVR& c = g[RC4];
    SpuOp op = SPU_fms;
    for (int i = 0; i < 4; ++i) {
        float x = ftz(a.f32[i]), y = ftz(b.f32[i]), z = ftz(c.f32[i]);
        float v = op == SPU_fma ? std::fma(x, y, z) : op == SPU_fms ? std::fma(x, y, -z) : -std::fma(x, y, -z);
        r.f32[i] = ftz(v);
    }
    g[RT4] = r;
    return 0;
}

SPU_IMPL(fnms) {
    SPU_PROLOGUE;
    const PPCVR& c = g[RC4];
    SpuOp op = SPU_fnms;
    for (int i = 0; i < 4; ++i) {
        float x = ftz(a.f32[i]), y = ftz(b.f32[i]), z = ftz(c.f32[i]);
        float v = op == SPU_fma ? std::fma(x, y, z) : op == SPU_fms ? std::fma(x, y, -z) : -std::fma(x, y, -z);
        r.f32[i] = ftz(v);
    }
    g[RT4] = r;
    return 0;
}

SPU_IMPL(ila) {
    SPU_PROLOGUE;
    for (auto& e : rt.u32) e = (w >> 7) & 0x3FFFF;
    return 0;
}

SPU_IMPL(hbra) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(hbrr) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(ori) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] | (uint32_t)I10;
    return 0;
}

SPU_IMPL(ohi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] | (uint16_t)I10;
    return 0;
}

SPU_IMPL(orbi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] | (uint8_t)I10;
    return 0;
}

SPU_IMPL(andi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] & (uint32_t)I10;
    return 0;
}

SPU_IMPL(andhi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] & (uint16_t)I10;
    return 0;
}

SPU_IMPL(andbi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] & (uint8_t)I10;
    return 0;
}

SPU_IMPL(xori) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] ^ (uint32_t)I10;
    return 0;
}

SPU_IMPL(xorhi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] ^ (uint16_t)I10;
    return 0;
}

SPU_IMPL(xorbi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] ^ (uint8_t)I10;
    return 0;
}

SPU_IMPL(ai) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] + (uint32_t)I10;
    return 0;
}

SPU_IMPL(ahi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)(a.u16[i] + I10);
    return 0;
}

SPU_IMPL(sfi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)I10 - a.u32[i];
    return 0;
}

SPU_IMPL(sfhi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)(I10 - a.u16[i]);
    return 0;
}

SPU_IMPL(cgti) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (int32_t)a.u32[i] > I10 ? ~0u : 0;
    return 0;
}

SPU_IMPL(cgthi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = (int16_t)a.u16[i] > (int16_t)I10 ? 0xFFFF : 0;
    return 0;
}

SPU_IMPL(cgtbi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = (int8_t)a.u8[i] > (int8_t)I10 ? 0xFF : 0;
    return 0;
}

SPU_IMPL(clgti) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] > (uint32_t)I10 ? ~0u : 0;
    return 0;
}

SPU_IMPL(clgthi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] > (uint16_t)I10 ? 0xFFFF : 0;
    return 0;
}

SPU_IMPL(clgtbi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] > (uint8_t)I10 ? 0xFF : 0;
    return 0;
}

SPU_IMPL(ceqi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] == (uint32_t)I10 ? ~0u : 0;
    return 0;
}

SPU_IMPL(ceqhi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] == (uint16_t)I10 ? 0xFFFF : 0;
    return 0;
}

SPU_IMPL(ceqbi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] == (uint8_t)I10 ? 0xFF : 0;
    return 0;
}

SPU_IMPL(mpyi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int16_t)a.u32[i] * I10);
    return 0;
}

SPU_IMPL(mpyui) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(uint16_t)a.u32[i] * (uint16_t)I10;
    return 0;
}

SPU_IMPL(lqd) {
    SPU_PROLOGUE;
    lq(t, rt, VW(a, 0) + ((uint32_t)I10 << 4));
    return 0;
}

SPU_IMPL(stqd) {
    SPU_PROLOGUE;
    stq(t, rt, VW(a, 0) + ((uint32_t)I10 << 4));
    return 0;
}

SPU_IMPL(hgti) {
    SPU_PROLOGUE;
    SpuOp op = SPU_hgti;
    bool h = op == SPU_heqi ? VW(a, 0) == (uint32_t)I10
           : op == SPU_hgti ? (int32_t)VW(a, 0) > I10 : VW(a, 0) > (uint32_t)I10;
    if (h) {
        spu_dump_state(t, pc, "halt instruction");
        t.pc = pc;
        return SPU_EXIT(0xFFFFFFFF);
    }
    return 0;
}

SPU_IMPL(hlgti) {
    SPU_PROLOGUE;
    SpuOp op = SPU_hlgti;
    bool h = op == SPU_heqi ? VW(a, 0) == (uint32_t)I10
           : op == SPU_hgti ? (int32_t)VW(a, 0) > I10 : VW(a, 0) > (uint32_t)I10;
    if (h) {
        spu_dump_state(t, pc, "halt instruction");
        t.pc = pc;
        return SPU_EXIT(0xFFFFFFFF);
    }
    return 0;
}

SPU_IMPL(heqi) {
    SPU_PROLOGUE;
    SpuOp op = SPU_heqi;
    bool h = op == SPU_heqi ? VW(a, 0) == (uint32_t)I10
           : op == SPU_hgti ? (int32_t)VW(a, 0) > I10 : VW(a, 0) > (uint32_t)I10;
    if (h) {
        spu_dump_state(t, pc, "halt instruction");
        t.pc = pc;
        return SPU_EXIT(0xFFFFFFFF);
    }
    return 0;
}

SPU_IMPL(il) {
    SPU_PROLOGUE;
    for (auto& e : rt.u32) e = (uint32_t)I16;
    return 0;
}

SPU_IMPL(ilh) {
    SPU_PROLOGUE;
    for (auto& e : rt.u16) e = (uint16_t)U16;
    return 0;
}

SPU_IMPL(ilhu) {
    SPU_PROLOGUE;
    for (auto& e : rt.u32) e = U16 << 16;
    return 0;
}

SPU_IMPL(iohl) {
    SPU_PROLOGUE;
    for (auto& e : rt.u32) e |= U16;
    return 0;
}

SPU_IMPL(fsmbi) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) VB(rt, i) = (U16 >> (15 - i)) & 1 ? 0xFF : 0;
    return 0;
}

SPU_IMPL(lqa) {
    SPU_PROLOGUE;
    lq(t, rt, (uint32_t)I16 << 2);
    return 0;
}

SPU_IMPL(stqa) {
    SPU_PROLOGUE;
    stq(t, rt, (uint32_t)I16 << 2);
    return 0;
}

SPU_IMPL(lqr) {
    SPU_PROLOGUE;
    lq(t, rt, pc + ((uint32_t)I16 << 2));
    return 0;
}

SPU_IMPL(stqr) {
    SPU_PROLOGUE;
    stq(t, rt, pc + ((uint32_t)I16 << 2));
    return 0;
}

SPU_IMPL(br) {
    SPU_PROLOGUE;
    npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC;
    return 0;
}

SPU_IMPL(bra) {
    SPU_PROLOGUE;
    npc = ((uint32_t)I16 << 2) & 0x3FFFC;
    return 0;
}

SPU_IMPL(brsl) {
    SPU_PROLOGUE;
    memset(&rt, 0, 16);
    VW(rt, 0) = npc;
    npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC;
    return 0;
}

SPU_IMPL(brasl) {
    SPU_PROLOGUE;
    memset(&rt, 0, 16);
    VW(rt, 0) = npc;
    npc = ((uint32_t)I16 << 2) & 0x3FFFC;
    return 0;
}

SPU_IMPL(brz) {
    SPU_PROLOGUE;
    if (VW(rt, 0) == 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC;
    return 0;
}

SPU_IMPL(brnz) {
    SPU_PROLOGUE;
    if (VW(rt, 0) != 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC;
    return 0;
}

SPU_IMPL(brhz) {
    SPU_PROLOGUE;
    if (VH(rt, 1) == 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC;
    return 0;
}

SPU_IMPL(brhnz) {
    SPU_PROLOGUE;
    if (VH(rt, 1) != 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC;
    return 0;
}

SPU_IMPL(cflts) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) r.u32[i] = sat_f2i(std::ldexp((double)a.f32[i], 173 - (int)I8), true);
    rt = r;
    return 0;
}

SPU_IMPL(cfltu) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) r.u32[i] = sat_f2i(std::ldexp((double)a.f32[i], 173 - (int)I8), false);
    rt = r;
    return 0;
}

SPU_IMPL(csflt) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) r.f32[i] = (float)std::ldexp((double)(int32_t)a.u32[i], (int)I8 - 155);
    rt = r;
    return 0;
}

SPU_IMPL(cuflt) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) r.f32[i] = (float)std::ldexp((double)a.u32[i], (int)I8 - 155);
    rt = r;
    return 0;
}

SPU_IMPL(stop) {
    SPU_PROLOGUE;
    t.pc = pc;
    if (!spu_stop(t, w & 0x3FFF)) return SPU_EXIT(w & 0x3FFF);
    return 0;
}

SPU_IMPL(stopd) {
    SPU_PROLOGUE;
    spu_dump_state(t, pc, "stopd");
    t.pc = pc;
    return SPU_EXIT(0xFFFFFFFE);
}

SPU_IMPL(lnop) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(nop) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(sync) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(dsync) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(hbr) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(mfspr) {
    SPU_PROLOGUE;
    memset(&rt, 0, 16);
    return 0;
}

SPU_IMPL(mtspr) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(fscrrd) {
    SPU_PROLOGUE;
    memset(&rt, 0, 16);
    return 0;
}

SPU_IMPL(fscrwr) {
    SPU_PROLOGUE;
    return 0;
}

SPU_IMPL(rdch) {
    SPU_PROLOGUE;
    uint32_t v;
    t.pc = pc;
    if (!spu_read_channel(t, RA, v)) return SPU_EXIT(0);
    memset(&rt, 0, 16);
    VW(rt, 0) = v;
    return 0;
}

SPU_IMPL(rchcnt) {
    SPU_PROLOGUE;
    memset(&rt, 0, 16);
    VW(rt, 0) = spu_channel_count(t, RA);
    return 0;
}

SPU_IMPL(wrch) {
    SPU_PROLOGUE;
    t.pc = pc;
    if (!spu_write_channel(t, RA, VW(rt, 0))) return SPU_EXIT(0);
    return 0;
}

SPU_IMPL(bi) {
    SPU_PROLOGUE;
    npc = VW(a, 0) & 0x3FFFC;
    return 0;
}

SPU_IMPL(iret) {
    SPU_PROLOGUE;
    npc = t.srr0 & 0x3FFFC;
    return 0;
}

SPU_IMPL(bisl) {
    SPU_PROLOGUE;
    uint32_t target = VW(a, 0) & 0x3FFFC;
    memset(&rt, 0, 16);
    VW(rt, 0) = npc;
    npc = target;
    return 0;
}

SPU_IMPL(bisled) {
    SPU_PROLOGUE;
    uint32_t target = VW(a, 0) & 0x3FFFC;
    memset(&rt, 0, 16);
    VW(rt, 0) = npc;
    npc = target;
    return 0;
}

SPU_IMPL(biz) {
    SPU_PROLOGUE;
    if (VW(rt, 0) == 0) npc = VW(a, 0) & 0x3FFFC;
    return 0;
}

SPU_IMPL(binz) {
    SPU_PROLOGUE;
    if (VW(rt, 0) != 0) npc = VW(a, 0) & 0x3FFFC;
    return 0;
}

SPU_IMPL(bihz) {
    SPU_PROLOGUE;
    if (VH(rt, 1) == 0) npc = VW(a, 0) & 0x3FFFC;
    return 0;
}

SPU_IMPL(bihnz) {
    SPU_PROLOGUE;
    if (VH(rt, 1) != 0) npc = VW(a, 0) & 0x3FFFC;
    return 0;
}

SPU_IMPL(lqx) {
    SPU_PROLOGUE;
    lq(t, rt, VW(a, 0) + VW(b, 0));
    return 0;
}

SPU_IMPL(stqx) {
    SPU_PROLOGUE;
    stq(t, rt, VW(a, 0) + VW(b, 0));
    return 0;
}

SPU_IMPL(a) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] + b.u32[i];
    return 0;
}

SPU_IMPL(ah) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] + b.u16[i];
    return 0;
}

SPU_IMPL(sf) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = b.u32[i] - a.u32[i];
    return 0;
}

SPU_IMPL(sfh) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = b.u16[i] - a.u16[i];
    return 0;
}

SPU_IMPL(cg) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(((uint64_t)a.u32[i] + b.u32[i]) >> 32);
    return 0;
}

SPU_IMPL(bg) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = b.u32[i] >= a.u32[i] ? 1 : 0;
    return 0;
}

SPU_IMPL(addx) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] + b.u32[i] + (rt.u32[i] & 1);
    return 0;
}

SPU_IMPL(sfx) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = b.u32[i] + ~a.u32[i] + (rt.u32[i] & 1);
    return 0;
}

SPU_IMPL(cgx) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(((uint64_t)a.u32[i] + b.u32[i] + (rt.u32[i] & 1)) >> 32);
    return 0;
}

SPU_IMPL(bgx) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i)
        rt.u32[i] = ((int64_t)b.u32[i] - a.u32[i] - (1 - (rt.u32[i] & 1))) >= 0 ? 1 : 0;
    return 0;
}

SPU_IMPL(mpy) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int16_t)a.u32[i] * (int16_t)b.u32[i]);
    return 0;
}

SPU_IMPL(mpyu) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(uint16_t)a.u32[i] * (uint16_t)b.u32[i];
    return 0;
}

SPU_IMPL(mpyh) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = ((a.u32[i] >> 16) * (uint16_t)b.u32[i]) << 16;
    return 0;
}

SPU_IMPL(mpyhh) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int16_t)(a.u32[i] >> 16) * (int16_t)(b.u32[i] >> 16));
    return 0;
}

SPU_IMPL(mpyhhu) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (a.u32[i] >> 16) * (b.u32[i] >> 16);
    return 0;
}

SPU_IMPL(mpys) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(((int16_t)a.u32[i] * (int16_t)b.u32[i]) >> 16);
    return 0;
}

SPU_IMPL(mpyhha) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] += (uint32_t)((int16_t)(a.u32[i] >> 16) * (int16_t)(b.u32[i] >> 16));
    return 0;
}

SPU_IMPL(mpyhhau) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] += (a.u32[i] >> 16) * (b.u32[i] >> 16);
    return 0;
}

SPU_IMPL(and) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] & b.u64[i];
    return 0;
}

SPU_IMPL(or) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] | b.u64[i];
    return 0;
}

SPU_IMPL(xor) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] ^ b.u64[i];
    return 0;
}

SPU_IMPL(nand) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = ~(a.u64[i] & b.u64[i]);
    return 0;
}

SPU_IMPL(nor) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = ~(a.u64[i] | b.u64[i]);
    return 0;
}

SPU_IMPL(andc) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] & ~b.u64[i];
    return 0;
}

SPU_IMPL(orc) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] | ~b.u64[i];
    return 0;
}

SPU_IMPL(eqv) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = ~(a.u64[i] ^ b.u64[i]);
    return 0;
}

SPU_IMPL(orx) {
    SPU_PROLOGUE;
    r = PPCVR{};
    VW(r, 0) = a.u32[0] | a.u32[1] | a.u32[2] | a.u32[3];
    rt = r;
    return 0;
}

SPU_IMPL(ceq) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] == b.u32[i] ? ~0u : 0;
    return 0;
}

SPU_IMPL(ceqh) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] == b.u16[i] ? 0xFFFF : 0;
    return 0;
}

SPU_IMPL(ceqb) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] == b.u8[i] ? 0xFF : 0;
    return 0;
}

SPU_IMPL(cgt) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (int32_t)a.u32[i] > (int32_t)b.u32[i] ? ~0u : 0;
    return 0;
}

SPU_IMPL(cgth) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = (int16_t)a.u16[i] > (int16_t)b.u16[i] ? 0xFFFF : 0;
    return 0;
}

SPU_IMPL(cgtb) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = (int8_t)a.u8[i] > (int8_t)b.u8[i] ? 0xFF : 0;
    return 0;
}

SPU_IMPL(clgt) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] > b.u32[i] ? ~0u : 0;
    return 0;
}

SPU_IMPL(clgth) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] > b.u16[i] ? 0xFFFF : 0;
    return 0;
}

SPU_IMPL(clgtb) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] > b.u8[i] ? 0xFF : 0;
    return 0;
}

SPU_IMPL(heq) {
    SPU_PROLOGUE;
    SpuOp op = SPU_heq;
    bool h = op == SPU_heq ? VW(a, 0) == VW(b, 0)
           : op == SPU_hgt ? (int32_t)VW(a, 0) > (int32_t)VW(b, 0) : VW(a, 0) > VW(b, 0);
    if (h) {
        spu_dump_state(t, pc, "halt instruction");
        t.pc = pc;
        return SPU_EXIT(0xFFFFFFFF);
    }
    return 0;
}

SPU_IMPL(hgt) {
    SPU_PROLOGUE;
    SpuOp op = SPU_hgt;
    bool h = op == SPU_heq ? VW(a, 0) == VW(b, 0)
           : op == SPU_hgt ? (int32_t)VW(a, 0) > (int32_t)VW(b, 0) : VW(a, 0) > VW(b, 0);
    if (h) {
        spu_dump_state(t, pc, "halt instruction");
        t.pc = pc;
        return SPU_EXIT(0xFFFFFFFF);
    }
    return 0;
}

SPU_IMPL(hlgt) {
    SPU_PROLOGUE;
    SpuOp op = SPU_hlgt;
    bool h = op == SPU_heq ? VW(a, 0) == VW(b, 0)
           : op == SPU_hgt ? (int32_t)VW(a, 0) > (int32_t)VW(b, 0) : VW(a, 0) > VW(b, 0);
    if (h) {
        spu_dump_state(t, pc, "halt instruction");
        t.pc = pc;
        return SPU_EXIT(0xFFFFFFFF);
    }
    return 0;
}

SPU_IMPL(shl) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) { uint32_t n = b.u32[i] & 0x3F; rt.u32[i] = n > 31 ? 0 : a.u32[i] << n; }
    return 0;
}

SPU_IMPL(shlh) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) { uint32_t n = b.u16[i] & 0x1F; rt.u16[i] = n > 15 ? 0 : (uint16_t)(a.u16[i] << n); }
    return 0;
}

SPU_IMPL(shli) {
    SPU_PROLOGUE;
    uint32_t n = I7 & 0x3F; for (int i = 0; i < 4; ++i) rt.u32[i] = n > 31 ? 0 : a.u32[i] << n;
    return 0;
}

SPU_IMPL(shlhi) {
    SPU_PROLOGUE;
    uint32_t n = I7 & 0x1F; for (int i = 0; i < 8; ++i) rt.u16[i] = n > 15 ? 0 : (uint16_t)(a.u16[i] << n);
    return 0;
}

SPU_IMPL(rot) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = ppc_rotl32(a.u32[i], b.u32[i] & 31);
    return 0;
}

SPU_IMPL(roth) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) { uint32_t n = b.u16[i] & 15; rt.u16[i] = (uint16_t)((a.u16[i] << n) | (a.u16[i] >> ((16 - n) & 15))); }
    return 0;
}

SPU_IMPL(roti) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = ppc_rotl32(a.u32[i], I7 & 31);
    return 0;
}

SPU_IMPL(rothi) {
    SPU_PROLOGUE;
    uint32_t n = I7 & 15; for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)((a.u16[i] << n) | (a.u16[i] >> ((16 - n) & 15)));
    return 0;
}

SPU_IMPL(rotm) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) { uint32_t n = (0 - b.u32[i]) & 0x3F; rt.u32[i] = n > 31 ? 0 : a.u32[i] >> n; }
    return 0;
}

SPU_IMPL(rothm) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) { uint32_t n = (0 - b.u16[i]) & 0x1F; rt.u16[i] = n > 15 ? 0 : a.u16[i] >> n; }
    return 0;
}

SPU_IMPL(rotmi) {
    SPU_PROLOGUE;
    uint32_t n = (0 - I7) & 0x3F; for (int i = 0; i < 4; ++i) rt.u32[i] = n > 31 ? 0 : a.u32[i] >> n;
    return 0;
}

SPU_IMPL(rothmi) {
    SPU_PROLOGUE;
    uint32_t n = (0 - I7) & 0x1F; for (int i = 0; i < 8; ++i) rt.u16[i] = n > 15 ? 0 : a.u16[i] >> n;
    return 0;
}

SPU_IMPL(rotma) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) { uint32_t n = (0 - b.u32[i]) & 0x3F; rt.u32[i] = (uint32_t)((int32_t)a.u32[i] >> (n > 31 ? 31 : n)); }
    return 0;
}

SPU_IMPL(rotmah) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) { uint32_t n = (0 - b.u16[i]) & 0x1F; rt.u16[i] = (uint16_t)((int16_t)a.u16[i] >> (n > 15 ? 15 : n)); }
    return 0;
}

SPU_IMPL(rotmai) {
    SPU_PROLOGUE;
    uint32_t n = (0 - I7) & 0x3F; for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int32_t)a.u32[i] >> (n > 31 ? 31 : n));
    return 0;
}

SPU_IMPL(rotmahi) {
    SPU_PROLOGUE;
    uint32_t n = (0 - I7) & 0x1F; for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)((int16_t)a.u16[i] >> (n > 15 ? 15 : n));
    return 0;
}

SPU_IMPL(shlqby) {
    SPU_PROLOGUE;
    uint32_t n = VW(b, 0) & 0x1F; setq(rt, n > 15 ? 0 : q(a) << (8 * n));
    return 0;
}

SPU_IMPL(shlqbyi) {
    SPU_PROLOGUE;
    uint32_t n = I7 & 0x1F; setq(rt, n > 15 ? 0 : q(a) << (8 * n));
    return 0;
}

SPU_IMPL(shlqbybi) {
    SPU_PROLOGUE;
    uint32_t n = (VW(b, 0) >> 3) & 0x1F; setq(rt, n > 15 ? 0 : q(a) << (8 * n));
    return 0;
}

SPU_IMPL(rotqby) {
    SPU_PROLOGUE;
    uint32_t n = (VW(b, 0) & 15) * 8; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x);
    return 0;
}

SPU_IMPL(rotqbyi) {
    SPU_PROLOGUE;
    uint32_t n = (I7 & 15) * 8; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x);
    return 0;
}

SPU_IMPL(rotqbybi) {
    SPU_PROLOGUE;
    uint32_t n = ((VW(b, 0) >> 3) & 15) * 8; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x);
    return 0;
}

SPU_IMPL(rotqmby) {
    SPU_PROLOGUE;
    uint32_t n = (0 - VW(b, 0)) & 0x1F; setq(rt, n > 15 ? 0 : q(a) >> (8 * n));
    return 0;
}

SPU_IMPL(rotqmbyi) {
    SPU_PROLOGUE;
    uint32_t n = (0 - I7) & 0x1F; setq(rt, n > 15 ? 0 : q(a) >> (8 * n));
    return 0;
}

SPU_IMPL(rotqmbybi) {
    SPU_PROLOGUE;
    uint32_t n = (0 - (VW(b, 0) >> 3)) & 0x1F; setq(rt, n > 15 ? 0 : q(a) >> (8 * n));
    return 0;
}

SPU_IMPL(shlqbi) {
    SPU_PROLOGUE;
    setq(rt, q(a) << (VW(b, 0) & 7));
    return 0;
}

SPU_IMPL(shlqbii) {
    SPU_PROLOGUE;
    setq(rt, q(a) << (I7 & 7));
    return 0;
}

SPU_IMPL(rotqbi) {
    SPU_PROLOGUE;
    uint32_t n = VW(b, 0) & 7; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x);
    return 0;
}

SPU_IMPL(rotqbii) {
    SPU_PROLOGUE;
    uint32_t n = I7 & 7; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x);
    return 0;
}

SPU_IMPL(rotqmbi) {
    SPU_PROLOGUE;
    setq(rt, q(a) >> ((0 - VW(b, 0)) & 7));
    return 0;
}

SPU_IMPL(rotqmbii) {
    SPU_PROLOGUE;
    setq(rt, q(a) >> ((0 - I7) & 7));
    return 0;
}

SPU_IMPL(cbd) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + I7, 1);
    return 0;
}

SPU_IMPL(chd) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + I7, 2);
    return 0;
}

SPU_IMPL(cwd) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + I7, 4);
    return 0;
}

SPU_IMPL(cdd) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + I7, 8);
    return 0;
}

SPU_IMPL(cbx) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + VW(b, 0), 1);
    return 0;
}

SPU_IMPL(chx) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + VW(b, 0), 2);
    return 0;
}

SPU_IMPL(cwx) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + VW(b, 0), 4);
    return 0;
}

SPU_IMPL(cdx) {
    SPU_PROLOGUE;
    insert_ctrl(rt, VW(a, 0) + VW(b, 0), 8);
    return 0;
}

SPU_IMPL(fsm) {
    SPU_PROLOGUE;
    uint32_t m = VW(a, 0); for (int i = 0; i < 4; ++i) VW(r, i) = (m >> (3 - i)) & 1 ? ~0u : 0; rt = r;
    return 0;
}

SPU_IMPL(fsmh) {
    SPU_PROLOGUE;
    uint32_t m = VW(a, 0); for (int i = 0; i < 8; ++i) VH(r, i) = (m >> (7 - i)) & 1 ? 0xFFFF : 0; rt = r;
    return 0;
}

SPU_IMPL(fsmb) {
    SPU_PROLOGUE;
    uint32_t m = VW(a, 0); for (int i = 0; i < 16; ++i) VB(r, i) = (m >> (15 - i)) & 1 ? 0xFF : 0; rt = r;
    return 0;
}

SPU_IMPL(gb) {
    SPU_PROLOGUE;
    uint32_t m = 0; for (int i = 0; i < 4; ++i) m = (m << 1) | (VW(a, i) & 1); r = PPCVR{}; VW(r, 0) = m; rt = r;
    return 0;
}

SPU_IMPL(gbh) {
    SPU_PROLOGUE;
    uint32_t m = 0; for (int i = 0; i < 8; ++i) m = (m << 1) | (VH(a, i) & 1); r = PPCVR{}; VW(r, 0) = m; rt = r;
    return 0;
}

SPU_IMPL(gbb) {
    SPU_PROLOGUE;
    uint32_t m = 0; for (int i = 0; i < 16; ++i) m = (m << 1) | (VB(a, i) & 1); r = PPCVR{}; VW(r, 0) = m; rt = r;
    return 0;
}

SPU_IMPL(cntb) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = (uint8_t)__builtin_popcount(a.u8[i]);
    return 0;
}

SPU_IMPL(clz) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] ? __builtin_clz(a.u32[i]) : 32;
    return 0;
}

SPU_IMPL(avgb) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = (uint8_t)((a.u8[i] + b.u8[i] + 1) >> 1);
    return 0;
}

SPU_IMPL(absdb) {
    SPU_PROLOGUE;
    for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] > b.u8[i] ? a.u8[i] - b.u8[i] : b.u8[i] - a.u8[i];
    return 0;
}

SPU_IMPL(sumb) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) {
        uint16_t sb = 0, sa = 0;
        for (int j = 0; j < 4; ++j) {
            sb += VB(b, 4 * i + j);
            sa += VB(a, 4 * i + j);
        }
        VH(r, 2 * i) = sb;
        VH(r, 2 * i + 1) = sa;
    }
    rt = r;
    return 0;
}

SPU_IMPL(xsbh) {
    SPU_PROLOGUE;
    for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)(int16_t)(int8_t)a.u16[i];
    return 0;
}

SPU_IMPL(xshw) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(int32_t)(int16_t)a.u32[i];
    return 0;
}

SPU_IMPL(xswd) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) rt.u64[i] = (uint64_t)(int64_t)(int32_t)a.u64[i];
    return 0;
}

SPU_IMPL(fa) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.f32[i] = ftz(ftz(a.f32[i]) + ftz(b.f32[i]));
    return 0;
}

SPU_IMPL(fs) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.f32[i] = ftz(ftz(a.f32[i]) - ftz(b.f32[i]));
    return 0;
}

SPU_IMPL(fm) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.f32[i] = ftz(ftz(a.f32[i]) * ftz(b.f32[i]));
    return 0;
}

SPU_IMPL(fceq) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = ftz(a.f32[i]) == ftz(b.f32[i]) ? ~0u : 0;
    return 0;
}

SPU_IMPL(fcgt) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = ftz(a.f32[i]) > ftz(b.f32[i]) ? ~0u : 0;
    return 0;
}

SPU_IMPL(fcmeq) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = std::fabs(ftz(a.f32[i])) == std::fabs(ftz(b.f32[i])) ? ~0u : 0;
    return 0;
}

SPU_IMPL(fcmgt) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.u32[i] = std::fabs(ftz(a.f32[i])) > std::fabs(ftz(b.f32[i])) ? ~0u : 0;
    return 0;
}

SPU_IMPL(frest) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.f32[i] = 1.0f / ftz(a.f32[i]);
    return 0;
}

SPU_IMPL(frsqest) {
    SPU_PROLOGUE;
    for (int i = 0; i < 4; ++i) rt.f32[i] = 1.0f / std::sqrt(std::fabs(ftz(a.f32[i])));
    return 0;
}

SPU_IMPL(fi) {
    SPU_PROLOGUE;
    rt = b;  // estimates above are exact, so interpolation is the identity
    return 0;
}

SPU_IMPL(dfa) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfa;
    for (int i = 0; i < 2; ++i) {
        double x, y, z;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        memcpy(&z, &rt.u64[i], 8);
        double v = op == SPU_dfa ? x + y : op == SPU_dfs ? x - y : op == SPU_dfm ? x * y
                 : op == SPU_dfma ? std::fma(x, y, z) : op == SPU_dfms ? std::fma(x, y, -z)
                 : op == SPU_dfnms ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        memcpy(&r.u64[i], &v, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfs) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfs;
    for (int i = 0; i < 2; ++i) {
        double x, y, z;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        memcpy(&z, &rt.u64[i], 8);
        double v = op == SPU_dfa ? x + y : op == SPU_dfs ? x - y : op == SPU_dfm ? x * y
                 : op == SPU_dfma ? std::fma(x, y, z) : op == SPU_dfms ? std::fma(x, y, -z)
                 : op == SPU_dfnms ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        memcpy(&r.u64[i], &v, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfm) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfm;
    for (int i = 0; i < 2; ++i) {
        double x, y, z;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        memcpy(&z, &rt.u64[i], 8);
        double v = op == SPU_dfa ? x + y : op == SPU_dfs ? x - y : op == SPU_dfm ? x * y
                 : op == SPU_dfma ? std::fma(x, y, z) : op == SPU_dfms ? std::fma(x, y, -z)
                 : op == SPU_dfnms ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        memcpy(&r.u64[i], &v, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfma) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfma;
    for (int i = 0; i < 2; ++i) {
        double x, y, z;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        memcpy(&z, &rt.u64[i], 8);
        double v = op == SPU_dfa ? x + y : op == SPU_dfs ? x - y : op == SPU_dfm ? x * y
                 : op == SPU_dfma ? std::fma(x, y, z) : op == SPU_dfms ? std::fma(x, y, -z)
                 : op == SPU_dfnms ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        memcpy(&r.u64[i], &v, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfms) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfms;
    for (int i = 0; i < 2; ++i) {
        double x, y, z;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        memcpy(&z, &rt.u64[i], 8);
        double v = op == SPU_dfa ? x + y : op == SPU_dfs ? x - y : op == SPU_dfm ? x * y
                 : op == SPU_dfma ? std::fma(x, y, z) : op == SPU_dfms ? std::fma(x, y, -z)
                 : op == SPU_dfnms ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        memcpy(&r.u64[i], &v, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfnms) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfnms;
    for (int i = 0; i < 2; ++i) {
        double x, y, z;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        memcpy(&z, &rt.u64[i], 8);
        double v = op == SPU_dfa ? x + y : op == SPU_dfs ? x - y : op == SPU_dfm ? x * y
                 : op == SPU_dfma ? std::fma(x, y, z) : op == SPU_dfms ? std::fma(x, y, -z)
                 : op == SPU_dfnms ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        memcpy(&r.u64[i], &v, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfnma) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfnma;
    for (int i = 0; i < 2; ++i) {
        double x, y, z;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        memcpy(&z, &rt.u64[i], 8);
        double v = op == SPU_dfa ? x + y : op == SPU_dfs ? x - y : op == SPU_dfm ? x * y
                 : op == SPU_dfma ? std::fma(x, y, z) : op == SPU_dfms ? std::fma(x, y, -z)
                 : op == SPU_dfnms ? -std::fma(x, y, -z) : -std::fma(x, y, z);
        memcpy(&r.u64[i], &v, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfceq) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfceq;
    for (int i = 0; i < 2; ++i) {
        double x, y;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        if (op == SPU_dfcmeq || op == SPU_dfcmgt) x = std::fabs(x), y = std::fabs(y);
        bool c = (op == SPU_dfceq || op == SPU_dfcmeq) ? x == y : x > y;
        r.u64[i] = c ? ~0ull : 0;
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfcgt) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfcgt;
    for (int i = 0; i < 2; ++i) {
        double x, y;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        if (op == SPU_dfcmeq || op == SPU_dfcmgt) x = std::fabs(x), y = std::fabs(y);
        bool c = (op == SPU_dfceq || op == SPU_dfcmeq) ? x == y : x > y;
        r.u64[i] = c ? ~0ull : 0;
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfcmeq) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfcmeq;
    for (int i = 0; i < 2; ++i) {
        double x, y;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        if (op == SPU_dfcmeq || op == SPU_dfcmgt) x = std::fabs(x), y = std::fabs(y);
        bool c = (op == SPU_dfceq || op == SPU_dfcmeq) ? x == y : x > y;
        r.u64[i] = c ? ~0ull : 0;
    }
    rt = r;
    return 0;
}

SPU_IMPL(dfcmgt) {
    SPU_PROLOGUE;
    SpuOp op = SPU_dfcmgt;
    for (int i = 0; i < 2; ++i) {
        double x, y;
        memcpy(&x, &a.u64[i], 8);
        memcpy(&y, &b.u64[i], 8);
        if (op == SPU_dfcmeq || op == SPU_dfcmgt) x = std::fabs(x), y = std::fabs(y);
        bool c = (op == SPU_dfceq || op == SPU_dfcmeq) ? x == y : x > y;
        r.u64[i] = c ? ~0ull : 0;
    }
    rt = r;
    return 0;
}

SPU_IMPL(fesd) {
    SPU_PROLOGUE;
    for (int i = 0; i < 2; ++i) {
        double d = VF(a, 2 * i);
        memcpy(&r.u64[1 - i], &d, 8);
    }
    rt = r;
    return 0;
}

SPU_IMPL(frds) {
    SPU_PROLOGUE;
    r = PPCVR{};
    for (int i = 0; i < 2; ++i) {
        double d;
        memcpy(&d, &a.u64[1 - i], 8);
        VF(r, 2 * i) = (float)d;
    }
    rt = r;
    return 0;
}

SPU_IMPL(dftsv) {
    SPU_PROLOGUE;
    memset(&rt, 0, 16);
    return 0;
}

