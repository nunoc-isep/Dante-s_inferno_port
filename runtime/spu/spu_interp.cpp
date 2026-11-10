// SPU interpreter. Registers use the byte-reversed layout (see ppc_vmx.h):
// VW(r, 0) is the preferred word slot, q() gives the register as a 128-bit
// big-endian number.
#include <cmath>
#include <cstring>

#include "spu.h"

SpuOp g_spu_decode[2048];
const char* g_spu_op_names[] = {
#define SPU_OP(name, fmt, first, count) #name,
#include "spu_opcodes.inc"
#undef SPU_OP
    "invalid"};

void spu_init_decoder() {
    static bool done = false;
    if (done) return;
    for (auto& e : g_spu_decode) e = SPU_INVALID;
#define SPU_OP(name, fmt, first, count) \
    for (int i = 0; i < count; ++i) g_spu_decode[first + i] = SPU_##name;
#include "spu_opcodes.inc"
#undef SPU_OP
    done = true;
}

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
static inline float ftz(float f) { return std::fpclassify(f) == FP_SUBNORMAL ? std::copysign(0.0f, f) : f; }
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

// Debug aid: write LS + registers of a thread that stopped abnormally to spu_<id>_<pc>.{ls,regs}
static void dump_state(SpuThread& t, uint32_t pc, const char* why) {
    char fn[96];
    snprintf(fn, sizeof(fn), "spu_%08X_%05X.ls", t.id, pc);
    if (FILE* f = fopen(fn, "wb")) {
        fwrite(t.ls, 1, 0x40000, f);
        fclose(f);
    }
    snprintf(fn, sizeof(fn), "spu_%08X_%05X.regs", t.id, pc);
    if (FILE* f = fopen(fn, "w")) {
        fprintf(f, "%s at pc 0x%05X\n", why, pc);
        for (int i = 0; i < 128; ++i)
            fprintf(f, "$%-3d %08X %08X %08X %08X\n", i, VW(t.gpr[i], 0), VW(t.gpr[i], 1), VW(t.gpr[i], 2), VW(t.gpr[i], 3));
        fclose(f);
    }
    LOG_E("spu", "SPU 0x%X: %s at 0x%05X - local store and registers saved to spu_%08X_%05X.*", t.id, why, pc, t.id, pc);
}

std::string spu_disasm(uint32_t w, uint32_t pc) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%05X: %08X %s", pc, w, g_spu_op_names[g_spu_decode[w >> 21]]);
    return buf;
}

uint32_t spu_run(SpuThread& t) {
    spu_init_decoder();
    PPCVR* g = t.gpr;
    uint32_t pc = t.pc;
    for (;;) {
        if ((t.insn_count & 0xFFFF) == 0 && t.stop_req) {
            t.pc = pc;
            return 0;
        }
        ++t.insn_count;
        uint32_t w = ppc_ld32(t.ls + (pc & 0x3FFFC));
        uint32_t npc = (pc + 4) & 0x3FFFC;
        uint32_t RT = w & 0x7F, RA = (w >> 7) & 0x7F, RB = (w >> 14) & 0x7F;
        uint32_t RT4 = (w >> 21) & 0x7F, RC4 = w & 0x7F;  // RRR: rt is the top field, rc the bottom
        int32_t I7 = sx((w >> 14) & 0x7F, 7), I10 = sx((w >> 14) & 0x3FF, 10), I16 = sx((w >> 7) & 0xFFFF, 16);
        uint32_t U16 = (w >> 7) & 0xFFFF, I8 = (w >> 14) & 0xFF;
        PPCVR& rt = g[RT];
        const PPCVR a = g[RA], b = g[RB];
        PPCVR r;

        switch (g_spu_decode[w >> 21]) {
            // ---------------------------------------------------------------- RRR
            case SPU_selb: {
                const PPCVR& c = g[RC4];
                for (int i = 0; i < 2; ++i) r.u64[i] = (a.u64[i] & ~c.u64[i]) | (b.u64[i] & c.u64[i]);
                g[RT4] = r;
                break;
            }
            case SPU_shufb: {
                const PPCVR& c = g[RC4];
                for (int i = 0; i < 16; ++i) {
                    uint8_t s = VB(c, i);
                    if (s >= 0xE0) VB(r, i) = 0x80;
                    else if (s >= 0xC0) VB(r, i) = 0xFF;
                    else if (s >= 0x80) VB(r, i) = 0x00;
                    else VB(r, i) = (s & 0x10) ? VB(b, s & 15) : VB(a, s & 15);
                }
                g[RT4] = r;
                break;
            }
            case SPU_mpya: {
                const PPCVR& c = g[RC4];
                for (int i = 0; i < 4; ++i) r.u32[i] = (uint32_t)((int16_t)a.u32[i] * (int16_t)b.u32[i]) + c.u32[i];
                g[RT4] = r;
                break;
            }
            case SPU_fma: case SPU_fms: case SPU_fnms: {
                const PPCVR& c = g[RC4];
                SpuOp op = g_spu_decode[w >> 21];
                for (int i = 0; i < 4; ++i) {
                    float x = ftz(a.f32[i]), y = ftz(b.f32[i]), z = ftz(c.f32[i]);
                    float v = op == SPU_fma ? std::fma(x, y, z) : op == SPU_fms ? std::fma(x, y, -z) : -std::fma(x, y, -z);
                    r.f32[i] = ftz(v);
                }
                g[RT4] = r;
                break;
            }
            // ---------------------------------------------------------------- RI18
            case SPU_ila: for (auto& e : rt.u32) e = (w >> 7) & 0x3FFFF; break;
            case SPU_hbra: case SPU_hbrr: break;
            // ---------------------------------------------------------------- RI10
            case SPU_ori: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] | (uint32_t)I10; break;
            case SPU_ohi: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] | (uint16_t)I10; break;
            case SPU_orbi: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] | (uint8_t)I10; break;
            case SPU_andi: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] & (uint32_t)I10; break;
            case SPU_andhi: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] & (uint16_t)I10; break;
            case SPU_andbi: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] & (uint8_t)I10; break;
            case SPU_xori: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] ^ (uint32_t)I10; break;
            case SPU_xorhi: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] ^ (uint16_t)I10; break;
            case SPU_xorbi: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] ^ (uint8_t)I10; break;
            case SPU_ai: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] + (uint32_t)I10; break;
            case SPU_ahi: for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)(a.u16[i] + I10); break;
            case SPU_sfi: for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)I10 - a.u32[i]; break;
            case SPU_sfhi: for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)(I10 - a.u16[i]); break;
            case SPU_cgti: for (int i = 0; i < 4; ++i) rt.u32[i] = (int32_t)a.u32[i] > I10 ? ~0u : 0; break;
            case SPU_cgthi: for (int i = 0; i < 8; ++i) rt.u16[i] = (int16_t)a.u16[i] > (int16_t)I10 ? 0xFFFF : 0; break;
            case SPU_cgtbi: for (int i = 0; i < 16; ++i) rt.u8[i] = (int8_t)a.u8[i] > (int8_t)I10 ? 0xFF : 0; break;
            case SPU_clgti: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] > (uint32_t)I10 ? ~0u : 0; break;
            case SPU_clgthi: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] > (uint16_t)I10 ? 0xFFFF : 0; break;
            case SPU_clgtbi: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] > (uint8_t)I10 ? 0xFF : 0; break;
            case SPU_ceqi: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] == (uint32_t)I10 ? ~0u : 0; break;
            case SPU_ceqhi: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] == (uint16_t)I10 ? 0xFFFF : 0; break;
            case SPU_ceqbi: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] == (uint8_t)I10 ? 0xFF : 0; break;
            case SPU_mpyi: for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int16_t)a.u32[i] * I10); break;
            case SPU_mpyui: for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(uint16_t)a.u32[i] * (uint16_t)I10; break;
            case SPU_lqd: lq(t, rt, VW(a, 0) + ((uint32_t)I10 << 4)); break;
            case SPU_stqd: stq(t, rt, VW(a, 0) + ((uint32_t)I10 << 4)); break;
            case SPU_hgti: case SPU_hlgti: case SPU_heqi: {
                SpuOp op = g_spu_decode[w >> 21];
                bool h = op == SPU_heqi ? VW(a, 0) == (uint32_t)I10
                       : op == SPU_hgti ? (int32_t)VW(a, 0) > I10 : VW(a, 0) > (uint32_t)I10;
                if (h) {
                    dump_state(t, pc, "halt instruction");
                    t.pc = pc;
                    return 0xFFFFFFFF;
                }
                break;
            }
            // ---------------------------------------------------------------- RI16
            case SPU_il: for (auto& e : rt.u32) e = (uint32_t)I16; break;
            case SPU_ilh: for (auto& e : rt.u16) e = (uint16_t)U16; break;
            case SPU_ilhu: for (auto& e : rt.u32) e = U16 << 16; break;
            case SPU_iohl: for (auto& e : rt.u32) e |= U16; break;
            case SPU_fsmbi: for (int i = 0; i < 16; ++i) VB(rt, i) = (U16 >> (15 - i)) & 1 ? 0xFF : 0; break;
            case SPU_lqa: lq(t, rt, (uint32_t)I16 << 2); break;
            case SPU_stqa: stq(t, rt, (uint32_t)I16 << 2); break;
            case SPU_lqr: lq(t, rt, pc + ((uint32_t)I16 << 2)); break;
            case SPU_stqr: stq(t, rt, pc + ((uint32_t)I16 << 2)); break;
            case SPU_br: npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC; break;
            case SPU_bra: npc = ((uint32_t)I16 << 2) & 0x3FFFC; break;
            case SPU_brsl:
                memset(&rt, 0, 16);
                VW(rt, 0) = npc;
                npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC;
                break;
            case SPU_brasl:
                memset(&rt, 0, 16);
                VW(rt, 0) = npc;
                npc = ((uint32_t)I16 << 2) & 0x3FFFC;
                break;
            case SPU_brz: if (VW(rt, 0) == 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC; break;
            case SPU_brnz: if (VW(rt, 0) != 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC; break;
            case SPU_brhz: if (VH(rt, 1) == 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC; break;
            case SPU_brhnz: if (VH(rt, 1) != 0) npc = (pc + ((uint32_t)I16 << 2)) & 0x3FFFC; break;
            // ---------------------------------------------------------------- RI8
            case SPU_cflts:
                for (int i = 0; i < 4; ++i) r.u32[i] = sat_f2i(std::ldexp((double)a.f32[i], 173 - (int)I8), true);
                rt = r;
                break;
            case SPU_cfltu:
                for (int i = 0; i < 4; ++i) r.u32[i] = sat_f2i(std::ldexp((double)a.f32[i], 173 - (int)I8), false);
                rt = r;
                break;
            case SPU_csflt:
                for (int i = 0; i < 4; ++i) r.f32[i] = (float)std::ldexp((double)(int32_t)a.u32[i], (int)I8 - 155);
                rt = r;
                break;
            case SPU_cuflt:
                for (int i = 0; i < 4; ++i) r.f32[i] = (float)std::ldexp((double)a.u32[i], (int)I8 - 155);
                rt = r;
                break;
            // ---------------------------------------------------------------- control / channels
            case SPU_stop: {
                t.pc = pc;
                if (!spu_stop(t, w & 0x3FFF)) return w & 0x3FFF;
                break;
            }
            case SPU_stopd:
                dump_state(t, pc, "stopd");
                t.pc = pc;
                return 0xFFFFFFFE;
            case SPU_lnop: case SPU_nop: case SPU_sync: case SPU_dsync: case SPU_hbr: break;
            case SPU_mfspr: memset(&rt, 0, 16); break;
            case SPU_mtspr: break;
            case SPU_fscrrd: memset(&rt, 0, 16); break;
            case SPU_fscrwr: break;
            case SPU_rdch: {
                uint32_t v;
                t.pc = pc;
                if (!spu_read_channel(t, RA, v)) return 0;
                memset(&rt, 0, 16);
                VW(rt, 0) = v;
                break;
            }
            case SPU_rchcnt:
                memset(&rt, 0, 16);
                VW(rt, 0) = spu_channel_count(t, RA);
                break;
            case SPU_wrch:
                t.pc = pc;
                if (!spu_write_channel(t, RA, VW(rt, 0))) return 0;
                break;
            // ---------------------------------------------------------------- indirect branches
            case SPU_bi: npc = VW(a, 0) & 0x3FFFC; break;
            case SPU_iret: npc = t.srr0 & 0x3FFFC; break;
            case SPU_bisl: case SPU_bisled: {
                uint32_t target = VW(a, 0) & 0x3FFFC;
                memset(&rt, 0, 16);
                VW(rt, 0) = npc;
                npc = target;
                break;
            }
            case SPU_biz: if (VW(rt, 0) == 0) npc = VW(a, 0) & 0x3FFFC; break;
            case SPU_binz: if (VW(rt, 0) != 0) npc = VW(a, 0) & 0x3FFFC; break;
            case SPU_bihz: if (VH(rt, 1) == 0) npc = VW(a, 0) & 0x3FFFC; break;
            case SPU_bihnz: if (VH(rt, 1) != 0) npc = VW(a, 0) & 0x3FFFC; break;
            // ---------------------------------------------------------------- loads / stores (indexed)
            case SPU_lqx: lq(t, rt, VW(a, 0) + VW(b, 0)); break;
            case SPU_stqx: stq(t, rt, VW(a, 0) + VW(b, 0)); break;
            // ---------------------------------------------------------------- integer arithmetic
            case SPU_a: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] + b.u32[i]; break;
            case SPU_ah: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] + b.u16[i]; break;
            case SPU_sf: for (int i = 0; i < 4; ++i) rt.u32[i] = b.u32[i] - a.u32[i]; break;
            case SPU_sfh: for (int i = 0; i < 8; ++i) rt.u16[i] = b.u16[i] - a.u16[i]; break;
            case SPU_cg: for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(((uint64_t)a.u32[i] + b.u32[i]) >> 32); break;
            case SPU_bg: for (int i = 0; i < 4; ++i) rt.u32[i] = b.u32[i] >= a.u32[i] ? 1 : 0; break;
            case SPU_addx: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] + b.u32[i] + (rt.u32[i] & 1); break;
            case SPU_sfx: for (int i = 0; i < 4; ++i) rt.u32[i] = b.u32[i] + ~a.u32[i] + (rt.u32[i] & 1); break;
            case SPU_cgx:
                for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(((uint64_t)a.u32[i] + b.u32[i] + (rt.u32[i] & 1)) >> 32);
                break;
            case SPU_bgx:
                for (int i = 0; i < 4; ++i)
                    rt.u32[i] = ((int64_t)b.u32[i] - a.u32[i] - (1 - (rt.u32[i] & 1))) >= 0 ? 1 : 0;
                break;
            case SPU_mpy: for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int16_t)a.u32[i] * (int16_t)b.u32[i]); break;
            case SPU_mpyu: for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(uint16_t)a.u32[i] * (uint16_t)b.u32[i]; break;
            case SPU_mpyh: for (int i = 0; i < 4; ++i) rt.u32[i] = ((a.u32[i] >> 16) * (uint16_t)b.u32[i]) << 16; break;
            case SPU_mpyhh:
                for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int16_t)(a.u32[i] >> 16) * (int16_t)(b.u32[i] >> 16));
                break;
            case SPU_mpyhhu: for (int i = 0; i < 4; ++i) rt.u32[i] = (a.u32[i] >> 16) * (b.u32[i] >> 16); break;
            case SPU_mpys:
                for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(((int16_t)a.u32[i] * (int16_t)b.u32[i]) >> 16);
                break;
            case SPU_mpyhha:
                for (int i = 0; i < 4; ++i) rt.u32[i] += (uint32_t)((int16_t)(a.u32[i] >> 16) * (int16_t)(b.u32[i] >> 16));
                break;
            case SPU_mpyhhau: for (int i = 0; i < 4; ++i) rt.u32[i] += (a.u32[i] >> 16) * (b.u32[i] >> 16); break;
            // ---------------------------------------------------------------- logical
            case SPU_and: for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] & b.u64[i]; break;
            case SPU_or: for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] | b.u64[i]; break;
            case SPU_xor: for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] ^ b.u64[i]; break;
            case SPU_nand: for (int i = 0; i < 2; ++i) rt.u64[i] = ~(a.u64[i] & b.u64[i]); break;
            case SPU_nor: for (int i = 0; i < 2; ++i) rt.u64[i] = ~(a.u64[i] | b.u64[i]); break;
            case SPU_andc: for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] & ~b.u64[i]; break;
            case SPU_orc: for (int i = 0; i < 2; ++i) rt.u64[i] = a.u64[i] | ~b.u64[i]; break;
            case SPU_eqv: for (int i = 0; i < 2; ++i) rt.u64[i] = ~(a.u64[i] ^ b.u64[i]); break;
            case SPU_orx:
                r = PPCVR{};
                VW(r, 0) = a.u32[0] | a.u32[1] | a.u32[2] | a.u32[3];
                rt = r;
                break;
            // ---------------------------------------------------------------- compares
            case SPU_ceq: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] == b.u32[i] ? ~0u : 0; break;
            case SPU_ceqh: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] == b.u16[i] ? 0xFFFF : 0; break;
            case SPU_ceqb: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] == b.u8[i] ? 0xFF : 0; break;
            case SPU_cgt: for (int i = 0; i < 4; ++i) rt.u32[i] = (int32_t)a.u32[i] > (int32_t)b.u32[i] ? ~0u : 0; break;
            case SPU_cgth: for (int i = 0; i < 8; ++i) rt.u16[i] = (int16_t)a.u16[i] > (int16_t)b.u16[i] ? 0xFFFF : 0; break;
            case SPU_cgtb: for (int i = 0; i < 16; ++i) rt.u8[i] = (int8_t)a.u8[i] > (int8_t)b.u8[i] ? 0xFF : 0; break;
            case SPU_clgt: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] > b.u32[i] ? ~0u : 0; break;
            case SPU_clgth: for (int i = 0; i < 8; ++i) rt.u16[i] = a.u16[i] > b.u16[i] ? 0xFFFF : 0; break;
            case SPU_clgtb: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] > b.u8[i] ? 0xFF : 0; break;
            case SPU_heq: case SPU_hgt: case SPU_hlgt: {
                SpuOp op = g_spu_decode[w >> 21];
                bool h = op == SPU_heq ? VW(a, 0) == VW(b, 0)
                       : op == SPU_hgt ? (int32_t)VW(a, 0) > (int32_t)VW(b, 0) : VW(a, 0) > VW(b, 0);
                if (h) {
                    dump_state(t, pc, "halt instruction");
                    t.pc = pc;
                    return 0xFFFFFFFF;
                }
                break;
            }
            // ---------------------------------------------------------------- shifts / rotates (element)
            case SPU_shl: for (int i = 0; i < 4; ++i) { uint32_t n = b.u32[i] & 0x3F; rt.u32[i] = n > 31 ? 0 : a.u32[i] << n; } break;
            case SPU_shlh: for (int i = 0; i < 8; ++i) { uint32_t n = b.u16[i] & 0x1F; rt.u16[i] = n > 15 ? 0 : (uint16_t)(a.u16[i] << n); } break;
            case SPU_shli: { uint32_t n = I7 & 0x3F; for (int i = 0; i < 4; ++i) rt.u32[i] = n > 31 ? 0 : a.u32[i] << n; break; }
            case SPU_shlhi: { uint32_t n = I7 & 0x1F; for (int i = 0; i < 8; ++i) rt.u16[i] = n > 15 ? 0 : (uint16_t)(a.u16[i] << n); break; }
            case SPU_rot: for (int i = 0; i < 4; ++i) rt.u32[i] = ppc_rotl32(a.u32[i], b.u32[i] & 31); break;
            case SPU_roth: for (int i = 0; i < 8; ++i) { uint32_t n = b.u16[i] & 15; rt.u16[i] = (uint16_t)((a.u16[i] << n) | (a.u16[i] >> ((16 - n) & 15))); } break;
            case SPU_roti: for (int i = 0; i < 4; ++i) rt.u32[i] = ppc_rotl32(a.u32[i], I7 & 31); break;
            case SPU_rothi: { uint32_t n = I7 & 15; for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)((a.u16[i] << n) | (a.u16[i] >> ((16 - n) & 15))); break; }
            case SPU_rotm: for (int i = 0; i < 4; ++i) { uint32_t n = (0 - b.u32[i]) & 0x3F; rt.u32[i] = n > 31 ? 0 : a.u32[i] >> n; } break;
            case SPU_rothm: for (int i = 0; i < 8; ++i) { uint32_t n = (0 - b.u16[i]) & 0x1F; rt.u16[i] = n > 15 ? 0 : a.u16[i] >> n; } break;
            case SPU_rotmi: { uint32_t n = (0 - I7) & 0x3F; for (int i = 0; i < 4; ++i) rt.u32[i] = n > 31 ? 0 : a.u32[i] >> n; break; }
            case SPU_rothmi: { uint32_t n = (0 - I7) & 0x1F; for (int i = 0; i < 8; ++i) rt.u16[i] = n > 15 ? 0 : a.u16[i] >> n; break; }
            case SPU_rotma: for (int i = 0; i < 4; ++i) { uint32_t n = (0 - b.u32[i]) & 0x3F; rt.u32[i] = (uint32_t)((int32_t)a.u32[i] >> (n > 31 ? 31 : n)); } break;
            case SPU_rotmah: for (int i = 0; i < 8; ++i) { uint32_t n = (0 - b.u16[i]) & 0x1F; rt.u16[i] = (uint16_t)((int16_t)a.u16[i] >> (n > 15 ? 15 : n)); } break;
            case SPU_rotmai: { uint32_t n = (0 - I7) & 0x3F; for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)((int32_t)a.u32[i] >> (n > 31 ? 31 : n)); break; }
            case SPU_rotmahi: { uint32_t n = (0 - I7) & 0x1F; for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)((int16_t)a.u16[i] >> (n > 15 ? 15 : n)); break; }
            // ---------------------------------------------------------------- quadword shifts / rotates
            case SPU_shlqby: { uint32_t n = VW(b, 0) & 0x1F; setq(rt, n > 15 ? 0 : q(a) << (8 * n)); break; }
            case SPU_shlqbyi: { uint32_t n = I7 & 0x1F; setq(rt, n > 15 ? 0 : q(a) << (8 * n)); break; }
            case SPU_shlqbybi: { uint32_t n = (VW(b, 0) >> 3) & 0x1F; setq(rt, n > 15 ? 0 : q(a) << (8 * n)); break; }
            case SPU_rotqby: { uint32_t n = (VW(b, 0) & 15) * 8; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x); break; }
            case SPU_rotqbyi: { uint32_t n = (I7 & 15) * 8; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x); break; }
            case SPU_rotqbybi: { uint32_t n = ((VW(b, 0) >> 3) & 15) * 8; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x); break; }
            case SPU_rotqmby: { uint32_t n = (0 - VW(b, 0)) & 0x1F; setq(rt, n > 15 ? 0 : q(a) >> (8 * n)); break; }
            case SPU_rotqmbyi: { uint32_t n = (0 - I7) & 0x1F; setq(rt, n > 15 ? 0 : q(a) >> (8 * n)); break; }
            case SPU_rotqmbybi: { uint32_t n = (0 - (VW(b, 0) >> 3)) & 0x1F; setq(rt, n > 15 ? 0 : q(a) >> (8 * n)); break; }
            case SPU_shlqbi: setq(rt, q(a) << (VW(b, 0) & 7)); break;
            case SPU_shlqbii: setq(rt, q(a) << (I7 & 7)); break;
            case SPU_rotqbi: { uint32_t n = VW(b, 0) & 7; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x); break; }
            case SPU_rotqbii: { uint32_t n = I7 & 7; u128 x = q(a); setq(rt, n ? (x << n) | (x >> (128 - n)) : x); break; }
            case SPU_rotqmbi: setq(rt, q(a) >> ((0 - VW(b, 0)) & 7)); break;
            case SPU_rotqmbii: setq(rt, q(a) >> ((0 - I7) & 7)); break;
            // ---------------------------------------------------------------- shuffle-control generation
            case SPU_cbd: insert_ctrl(rt, VW(a, 0) + I7, 1); break;
            case SPU_chd: insert_ctrl(rt, VW(a, 0) + I7, 2); break;
            case SPU_cwd: insert_ctrl(rt, VW(a, 0) + I7, 4); break;
            case SPU_cdd: insert_ctrl(rt, VW(a, 0) + I7, 8); break;
            case SPU_cbx: insert_ctrl(rt, VW(a, 0) + VW(b, 0), 1); break;
            case SPU_chx: insert_ctrl(rt, VW(a, 0) + VW(b, 0), 2); break;
            case SPU_cwx: insert_ctrl(rt, VW(a, 0) + VW(b, 0), 4); break;
            case SPU_cdx: insert_ctrl(rt, VW(a, 0) + VW(b, 0), 8); break;
            // ---------------------------------------------------------------- form/gather masks, misc
            case SPU_fsm: { uint32_t m = VW(a, 0); for (int i = 0; i < 4; ++i) VW(r, i) = (m >> (3 - i)) & 1 ? ~0u : 0; rt = r; break; }
            case SPU_fsmh: { uint32_t m = VW(a, 0); for (int i = 0; i < 8; ++i) VH(r, i) = (m >> (7 - i)) & 1 ? 0xFFFF : 0; rt = r; break; }
            case SPU_fsmb: { uint32_t m = VW(a, 0); for (int i = 0; i < 16; ++i) VB(r, i) = (m >> (15 - i)) & 1 ? 0xFF : 0; rt = r; break; }
            case SPU_gb: { uint32_t m = 0; for (int i = 0; i < 4; ++i) m = (m << 1) | (VW(a, i) & 1); r = PPCVR{}; VW(r, 0) = m; rt = r; break; }
            case SPU_gbh: { uint32_t m = 0; for (int i = 0; i < 8; ++i) m = (m << 1) | (VH(a, i) & 1); r = PPCVR{}; VW(r, 0) = m; rt = r; break; }
            case SPU_gbb: { uint32_t m = 0; for (int i = 0; i < 16; ++i) m = (m << 1) | (VB(a, i) & 1); r = PPCVR{}; VW(r, 0) = m; rt = r; break; }
            case SPU_cntb: for (int i = 0; i < 16; ++i) rt.u8[i] = (uint8_t)__builtin_popcount(a.u8[i]); break;
            case SPU_clz: for (int i = 0; i < 4; ++i) rt.u32[i] = a.u32[i] ? __builtin_clz(a.u32[i]) : 32; break;
            case SPU_avgb: for (int i = 0; i < 16; ++i) rt.u8[i] = (uint8_t)((a.u8[i] + b.u8[i] + 1) >> 1); break;
            case SPU_absdb: for (int i = 0; i < 16; ++i) rt.u8[i] = a.u8[i] > b.u8[i] ? a.u8[i] - b.u8[i] : b.u8[i] - a.u8[i]; break;
            case SPU_sumb:
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
                break;
            case SPU_xsbh: for (int i = 0; i < 8; ++i) rt.u16[i] = (uint16_t)(int16_t)(int8_t)a.u16[i]; break;
            case SPU_xshw: for (int i = 0; i < 4; ++i) rt.u32[i] = (uint32_t)(int32_t)(int16_t)a.u32[i]; break;
            case SPU_xswd: for (int i = 0; i < 2; ++i) rt.u64[i] = (uint64_t)(int64_t)(int32_t)a.u64[i]; break;
            // ---------------------------------------------------------------- single precision
            case SPU_fa: for (int i = 0; i < 4; ++i) rt.f32[i] = ftz(ftz(a.f32[i]) + ftz(b.f32[i])); break;
            case SPU_fs: for (int i = 0; i < 4; ++i) rt.f32[i] = ftz(ftz(a.f32[i]) - ftz(b.f32[i])); break;
            case SPU_fm: for (int i = 0; i < 4; ++i) rt.f32[i] = ftz(ftz(a.f32[i]) * ftz(b.f32[i])); break;
            case SPU_fceq: for (int i = 0; i < 4; ++i) rt.u32[i] = ftz(a.f32[i]) == ftz(b.f32[i]) ? ~0u : 0; break;
            case SPU_fcgt: for (int i = 0; i < 4; ++i) rt.u32[i] = ftz(a.f32[i]) > ftz(b.f32[i]) ? ~0u : 0; break;
            case SPU_fcmeq: for (int i = 0; i < 4; ++i) rt.u32[i] = std::fabs(ftz(a.f32[i])) == std::fabs(ftz(b.f32[i])) ? ~0u : 0; break;
            case SPU_fcmgt: for (int i = 0; i < 4; ++i) rt.u32[i] = std::fabs(ftz(a.f32[i])) > std::fabs(ftz(b.f32[i])) ? ~0u : 0; break;
            case SPU_frest: for (int i = 0; i < 4; ++i) rt.f32[i] = 1.0f / ftz(a.f32[i]); break;
            case SPU_frsqest: for (int i = 0; i < 4; ++i) rt.f32[i] = 1.0f / std::sqrt(std::fabs(ftz(a.f32[i]))); break;
            case SPU_fi: rt = b; break;  // estimates above are exact, so interpolation is the identity
            // ---------------------------------------------------------------- double precision (doubles in BE dwords 0/1)
            case SPU_dfa: case SPU_dfs: case SPU_dfm: case SPU_dfma: case SPU_dfms: case SPU_dfnms: case SPU_dfnma: {
                SpuOp op = g_spu_decode[w >> 21];
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
                break;
            }
            case SPU_dfceq: case SPU_dfcgt: case SPU_dfcmeq: case SPU_dfcmgt: {
                SpuOp op = g_spu_decode[w >> 21];
                for (int i = 0; i < 2; ++i) {
                    double x, y;
                    memcpy(&x, &a.u64[i], 8);
                    memcpy(&y, &b.u64[i], 8);
                    if (op == SPU_dfcmeq || op == SPU_dfcmgt) x = std::fabs(x), y = std::fabs(y);
                    bool c = (op == SPU_dfceq || op == SPU_dfcmeq) ? x == y : x > y;
                    r.u64[i] = c ? ~0ull : 0;
                }
                rt = r;
                break;
            }
            case SPU_fesd:
                for (int i = 0; i < 2; ++i) {
                    double d = VF(a, 2 * i);
                    memcpy(&r.u64[1 - i], &d, 8);
                }
                rt = r;
                break;
            case SPU_frds:
                r = PPCVR{};
                for (int i = 0; i < 2; ++i) {
                    double d;
                    memcpy(&d, &a.u64[1 - i], 8);
                    VF(r, 2 * i) = (float)d;
                }
                rt = r;
                break;
            case SPU_dftsv: memset(&rt, 0, 16); break;

            case SPU_INVALID:
            default:
                dump_state(t, pc, "invalid instruction");
                t.pc = pc;
                return 0xFFFFFFFD;
        }
        pc = npc;
    }
}
