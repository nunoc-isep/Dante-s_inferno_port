// RSX (NV40) fragment program -> HLSL pixel shader.
// Words are stored big-endian with their 16-bit halves swapped; bitfields follow RPCS3's
// FragmentProgramDecompiler (OPDEST / SRC0 / SRC1 / SRC2).
#include <cstdio>
#include <cstring>
#include <string>

#include "../kernel/kernel.h"
#include "rsx_shaders.h"

namespace {

const char* kSwz = "xyzw";
const char* kAttr[16] = {"WPOS", "COL0", "COL1", "FOGC", "TEX0", "TEX1", "TEX2", "TEX3",
                         "TEX4", "TEX5", "TEX6", "TEX7", "TEX8", "TEX9", "SSA", "ATTR15"};

uint32_t fp_word(uint32_t addr) {
    uint32_t v = vm::read32(addr);
    return (v << 16) | (v >> 16);
}

float fp_float(uint32_t addr) {
    uint32_t v = fp_word(addr);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

struct FpInsn {
    uint32_t dst, s0, s1, s2;
    bool end() const { return dst & 1; }
    uint32_t dest_reg() const { return (dst >> 1) & 0x3F; }
    bool dst_fp16() const { return (dst >> 7) & 1; }
    bool set_cond() const { return (dst >> 8) & 1; }
    bool mask(int c) const { return (dst >> (9 + c)) & 1; }
    uint32_t attr() const { return (dst >> 13) & 0xF; }
    uint32_t tex_num() const { return (dst >> 17) & 0xF; }
    uint32_t opcode() const { return ((dst >> 24) & 0x3F) | (((s1 >> 31) & 1) << 6); }
    bool no_dest() const { return (dst >> 30) & 1; }
    bool saturate() const { return (dst >> 31) & 1; }
    bool exec_lt() const { return (s0 >> 18) & 1; }
    bool exec_eq() const { return (s0 >> 19) & 1; }
    bool exec_gt() const { return (s0 >> 20) & 1; }
    uint32_t cond_swz(int c) const { return (s0 >> (21 + 2 * c)) & 3; }
    uint32_t cond_mod_reg() const { return (s0 >> 30) & 1; }
    uint32_t cond_reg() const { return (s0 >> 31) & 1; }
    uint32_t scale() const { return (s1 >> 28) & 7; }
    uint32_t src(int i) const { return i == 0 ? s0 : i == 1 ? s1 : s2; }
    bool src_abs(int i) const { return i == 0 ? (s0 >> 29) & 1 : (src(i) >> 18) & 1; }
};

struct Ctx {
    uint32_t addr;
    uint32_t inputs = 0;
    uint32_t tex_used = 0;
    bool const_used = false;
    float consts[4] = {0, 0, 0, 0};
};

std::string src_str(const FpInsn& in, int i, Ctx& cx) {
    uint32_t s = in.src(i);
    uint32_t type = s & 3, idx = (s >> 2) & 0x3F;
    bool fp16 = (s >> 8) & 1;
    char buf[96];
    std::string r;
    switch (type) {
        case 0: snprintf(buf, sizeof(buf), "%s[%u]", fp16 ? "H" : "R", idx); r = buf; break;
        case 1: {
            uint32_t a = in.attr();
            cx.inputs |= 1u << a;
            r = std::string("in_") + kAttr[a];
            break;
        }
        case 2:
            cx.const_used = true;
            snprintf(buf, sizeof(buf), "float4(%.9g, %.9g, %.9g, %.9g)", cx.consts[0], cx.consts[1], cx.consts[2],
                     cx.consts[3]);
            r = buf;
            break;
        default: r = "float4(0,0,0,0)"; break;
    }
    char swz[6] = {'.', kSwz[(s >> 9) & 3], kSwz[(s >> 11) & 3], kSwz[(s >> 13) & 3], kSwz[(s >> 15) & 3], 0};
    if (strcmp(swz, ".xyzw")) r = "(" + r + ")" + swz;
    if (in.src_abs(i)) r = "abs(" + r + ")";
    if ((s >> 17) & 1) r = "(-" + r + ")";
    return r;
}

std::string cond_expr(const FpInsn& in, int c) {
    bool lt = in.exec_lt(), eq = in.exec_eq(), gt = in.exec_gt();
    if (lt && eq && gt) return "true";
    if (!lt && !eq && !gt) return "false";
    char v[32];
    snprintf(v, sizeof(v), "CC%u.%c", in.cond_reg(), kSwz[in.cond_swz(c)]);
    std::string e;
    auto add = [&](const char* op) { e += (e.empty() ? "" : " || ") + std::string("(") + v + op + "0.0)"; };
    if (lt) add(" < ");
    if (eq) add(" == ");
    if (gt) add(" > ");
    return e;
}

const char* tex_type(uint8_t dim) { return dim == 3 ? "Texture3D" : dim == 6 ? "TextureCube" : "Texture2D"; }

}  // namespace

std::string fp_to_hlsl(uint32_t addr, const FpConfig& cfg, uint32_t& size_bytes, uint32_t& tex_used) {
    Ctx cx;
    cx.addr = addr;
    std::string body;
    uint32_t pc = addr;
    static bool warned_flow = false;
    for (int n = 0; n < 2048; ++n) {
        FpInsn in{fp_word(pc), fp_word(pc + 4), fp_word(pc + 8), fp_word(pc + 12)};
        // an inline constant (if any source uses it) follows the instruction
        bool has_const = (in.s0 & 3) == 2 || (in.s1 & 3) == 2 || (in.s2 & 3) == 2;
        if (has_const)
            for (int k = 0; k < 4; ++k) cx.consts[k] = fp_float(pc + 16 + 4 * k);
        uint32_t op = in.opcode();
        std::string s0 = src_str(in, 0, cx), s1 = src_str(in, 1, cx), s2 = src_str(in, 2, cx);
        std::string x0 = "(" + s0 + ").x", x1 = "(" + s1 + ").x";
        std::string v;
        uint32_t t = in.tex_num();
        char tex[16];
        snprintf(tex, sizeof(tex), "tex%u", t);
        bool is_kil = false;
        switch (op) {
            case 0x00: break;                                   // NOP
            case 0x01: v = s0; break;                           // MOV
            case 0x02: v = s0 + " * " + s1; break;              // MUL
            case 0x03: v = s0 + " + " + s1; break;              // ADD
            case 0x04: v = s0 + " * " + s1 + " + " + s2; break; // MAD
            case 0x05: v = "dot((" + s0 + ").xyz, (" + s1 + ").xyz).xxxx"; break;
            case 0x06: v = "dot(" + s0 + ", " + s1 + ").xxxx"; break;
            case 0x07: v = "float4(1.0, (" + s0 + ").y * (" + s1 + ").y, (" + s0 + ").z, (" + s1 + ").w)"; break;
            case 0x08: v = "min(" + s0 + ", " + s1 + ")"; break;
            case 0x09: v = "max(" + s0 + ", " + s1 + ")"; break;
            case 0x0A: v = "float4((" + s0 + ") < (" + s1 + "))"; break;
            case 0x0B: v = "float4((" + s0 + ") >= (" + s1 + "))"; break;
            case 0x0C: v = "float4((" + s0 + ") <= (" + s1 + "))"; break;
            case 0x0D: v = "float4((" + s0 + ") > (" + s1 + "))"; break;
            case 0x0E: v = "float4((" + s0 + ") != (" + s1 + "))"; break;
            case 0x0F: v = "float4((" + s0 + ") == (" + s1 + "))"; break;
            case 0x10: v = "frac(" + s0 + ")"; break;
            case 0x11: v = "floor(" + s0 + ")"; break;
            case 0x12: is_kil = true; break;  // KIL
            case 0x15: v = "ddx(" + s0 + ")"; break;
            case 0x16: v = "ddy(" + s0 + ")"; break;
            case 0x17:  // TEX
                cx.tex_used |= 1u << t;
                v = cfg.tex_dims[t] == 3 || cfg.tex_dims[t] == 6 ? std::string(tex) + ".Sample(smp" + std::to_string(t) + ", (" + s0 + ").xyz)"
                                                                 : std::string(tex) + ".Sample(smp" + std::to_string(t) + ", (" + s0 + ").xy)";
                break;
            case 0x18:  // TXP (projective)
                cx.tex_used |= 1u << t;
                v = std::string(tex) + ".Sample(smp" + std::to_string(t) + ", (" + s0 + ").xy / (" + s0 + ").w)";
                break;
            case 0x19:  // TXD
                cx.tex_used |= 1u << t;
                v = std::string(tex) + ".SampleGrad(smp" + std::to_string(t) + ", (" + s0 + ").xy, (" + s1 + ").xy, (" + s2 + ").xy)";
                break;
            case 0x1A: v = "(1.0 / " + x0 + ").xxxx"; break;
            case 0x1B: v = "rsqrt(abs(" + x0 + ")).xxxx"; break;
            case 0x1C: v = "exp2(" + x0 + ").xxxx"; break;
            case 0x1D: v = "log2(" + x0 + ").xxxx"; break;
            case 0x1E: v = "fp_lit(" + s0 + ")"; break;
            case 0x1F: v = "lerp(" + s2 + ", " + s1 + ", " + s0 + ")"; break;  // LRP: s0*s1 + (1-s0)*s2
            case 0x20: v = "float4(1,1,1,1)"; break;
            case 0x21: v = "float4(0,0,0,0)"; break;
            case 0x22: v = "cos(" + x0 + ").xxxx"; break;
            case 0x23: v = "sin(" + x0 + ").xxxx"; break;
            case 0x24: v = "asfloat(f32tof16((" + s0 + ").x) | (f32tof16((" + s0 + ").y) << 16)).xxxx"; break;  // PK2
            case 0x25: v = "float4(f16tof32(asuint(" + x0 + ")), f16tof32(asuint(" + x0 + ") >> 16), f16tof32(asuint(" + x0 + ")), f16tof32(asuint(" + x0 + ") >> 16))"; break;  // UP2
            case 0x26: v = "pow(" + x0 + ", " + x1 + ").xxxx"; break;
            case 0x13: v = "fp_pk4(" + s0 + ")"; break;
            case 0x14: v = "fp_up4(" + x0 + ")"; break;
            case 0x27: v = "fp_pkb(" + s0 + ")"; break;
            case 0x28: v = "fp_upb(" + x0 + ")"; break;
            case 0x2E: v = "(dot((" + s0 + ").xy, (" + s1 + ").xy) + (" + s2 + ").x).xxxx"; break;  // DP2A
            case 0x2F:  // TXL
                cx.tex_used |= 1u << t;
                v = std::string(tex) + ".SampleLevel(smp" + std::to_string(t) + ", (" + s0 + ").xy, (" + s0 + ").w)";
                break;
            case 0x31:  // TXB
                cx.tex_used |= 1u << t;
                v = std::string(tex) + ".SampleBias(smp" + std::to_string(t) + ", (" + s0 + ").xy, (" + s0 + ").w)";
                break;
            case 0x36: v = "float4(reflect((" + s0 + ").xyz, normalize((" + s1 + ").xyz)), 0)"; break;  // REFL
            case 0x38: v = "dot((" + s0 + ").xy, (" + s1 + ").xy).xxxx"; break;  // DP2
            case 0x39: v = "float4(normalize((" + s0 + ").xyz), 0)"; break;          // NRM
            case 0x3A: v = s0 + " / " + x1; break;                                     // DIV
            case 0x3B: v = s0 + " * rsqrt(abs(" + x1 + "))"; break;                     // DIVSQ
            default:
                if (op >= 0x40) {
                    if (!warned_flow) LOG_W("rsx", "fragment program: flow control opcode 0x%X not supported yet", op);
                    warned_flow = true;
                } else {
                    LOG_W("rsx", "fragment program: opcode 0x%X not supported", op);
                }
                break;
        }
        if (is_kil) {
            body += "\tif (";
            for (int c = 0; c < 4; ++c) body += std::string(c ? " || " : "") + "(" + cond_expr(in, c) + ")";
            body += ") discard;\n";
        } else if (!v.empty()) {
            static const char* scales[8] = {"", " * 2.0", " * 4.0", " * 8.0", "", " / 2.0", " / 4.0", " / 8.0"};
            std::string val = "(" + v + ")" + scales[in.scale()];
            if (in.saturate()) val = "saturate(" + val + ")";
            body += "\t{ float4 t = " + val + ";\n";
            char dst[16];
            snprintf(dst, sizeof(dst), "%s[%u]", in.dst_fp16() ? "H" : "R", in.dest_reg());
            for (int c = 0; c < 4; ++c) {
                if (!in.mask(c)) continue;
                char m = kSwz[c];
                std::string cnd = cond_expr(in, c);
                std::string pre = cnd == "true" ? "\t  " : "\t  if (" + cnd + ") ";
                if (!in.no_dest()) body += pre + dst + "." + m + " = t." + m + ";\n";
                if (in.set_cond()) body += pre + "CC" + std::to_string(in.cond_mod_reg()) + "." + m + " = t." + m + ";\n";
            }
            body += "\t}\n";
        }
        pc += has_const ? 32 : 16;
        if (in.end()) break;
    }
    size_bytes = pc - addr;
    tex_used = cx.tex_used;

    std::string src;
    for (int t = 0; t < 16; ++t)
        if (cx.tex_used & (1u << t)) {
            src += std::string(tex_type(cfg.tex_dims[t])) + " tex" + std::to_string(t) + " : register(t" + std::to_string(t) + ");\n";
            src += "SamplerState smp" + std::to_string(t) + " : register(s" + std::to_string(t) + ");\n";
        }
    src += "struct PSIn { float4 pos : SV_Position;";
    for (int o = 1; o < 16; ++o) src += " float4 o" + std::to_string(o) + " : TEXCOORD" + std::to_string(o) + ";";
    src += " bool face : SV_IsFrontFace; };\n"
           "float4 fp_lit(float4 s) {\n"
           "\tfloat4 r = float4(1.0, max(s.x, 0.0), 0.0, 1.0);\n"
           "\tif (s.x > 0.0) r.z = pow(max(s.y, 0.0), s.w);\n"
           "\treturn r;\n}\n"
           "float4 fp_pk4(float4 v) { uint4 b = uint4(round(clamp(v, -1.0, 1.0) * 127.0)) & 0xFF; return asfloat(b.x | (b.y << 8) | (b.z << 16) | (b.w << 24)).xxxx; }\n"
           "float4 fp_up4(float f) { uint u = asuint(f); int4 b = int4(u << 24, u << 16, u << 8, u) >> 24; return float4(b) / 127.0; }\n"
           "float4 fp_pkb(float4 v) { uint4 b = uint4(round(saturate(v) * 255.0)); return asfloat(b.x | (b.y << 8) | (b.z << 16) | (b.w << 24)).xxxx; }\n"
           "float4 fp_upb(float f) { uint u = asuint(f); return float4(u & 0xFF, (u >> 8) & 0xFF, (u >> 16) & 0xFF, u >> 24) / 255.0; }\n"
           "struct PSOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;";
    bool depth = (cfg.shader_control & 0xE) != 0;
    if (depth) src += " float depth : SV_Depth;";
    src += " };\n"
           "PSOut main(PSIn i) {\n"
           "\tfloat4 R[64]; float4 H[96]; float4 CC0 = 0, CC1 = 0;\n"
           "\t[unroll] for (int k = 0; k < 64; ++k) R[k] = float4(0,0,0,0);\n"
           "\t[unroll] for (int k = 0; k < 96; ++k) H[k] = float4(0,0,0,0);\n"
           "\tfloat4 in_WPOS = i.pos, in_COL0 = i.o1, in_COL1 = i.o2, in_FOGC = i.o5;\n"
           "\tfloat4 in_TEX0 = i.o7, in_TEX1 = i.o8, in_TEX2 = i.o9, in_TEX3 = i.o10, in_TEX4 = i.o11, in_TEX5 = i.o12, in_TEX6 = i.o13, in_TEX7 = i.o14;\n"
           "\tfloat4 in_TEX8 = i.o15, in_TEX9 = i.o6, in_SSA = i.face ? 1.0 : -1.0, in_ATTR15 = 0;\n";
    src += body;
    bool r32 = (cfg.shader_control & 0x40) != 0;
    src += "\tPSOut o;\n";
    src += r32 ? "\to.c0 = R[0]; o.c1 = R[2]; o.c2 = R[3]; o.c3 = R[4];\n"
               : "\to.c0 = H[0]; o.c1 = H[4]; o.c2 = H[6]; o.c3 = H[8];\n";
    if (depth) src += "\to.depth = R[1].z;\n";
    if (cfg.alpha_test) {
        static const char* ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
        uint32_t f = cfg.alpha_func & 7;
        char buf[128];
        snprintf(buf, sizeof(buf), "\tif (!(o.c0.a %s %.9g)) discard;\n", ops[f], cfg.alpha_ref);
        if (f == 0) src += "\tdiscard;\n";
        else if (f != 7) src += buf;
    }
    src += "\treturn o;\n}\n";
    return src;
}
