// RSX (NV40) vertex program -> HLSL vertex shader.
// Instruction layout (4 x u32, LSB-first bitfields) follows RPCS3's VertexProgramDecompiler.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../kernel/kernel.h"
#include "rsx_shaders.h"

namespace {

const char* kSwz = "xyzw";

struct VpInsn {
    uint32_t d0, d1, d2, d3;
    // d0
    uint32_t addr_swz() const { return d0 & 3; }
    uint32_t cond_swz(int c) const { return (d0 >> (8 - 2 * c)) & 3; }  // c = 0..3 -> x..w
    uint32_t cond() const { return (d0 >> 10) & 7; }
    bool cond_test() const { return (d0 >> 13) & 1; }
    bool cond_update0() const { return (d0 >> 14) & 1; }
    uint32_t dst_tmp() const { return (d0 >> 15) & 0x3F; }
    bool src_abs(int i) const { return (d0 >> (21 + i)) & 1; }
    bool addr_reg_sel1() const { return (d0 >> 24) & 1; }
    bool cond_reg_sel1() const { return (d0 >> 25) & 1; }
    bool saturate() const { return (d0 >> 26) & 1; }
    bool cond_update1() const { return (d0 >> 29) & 1; }
    bool vec_result() const { return (d0 >> 30) & 1; }
    // d1
    uint32_t input_src() const { return (d1 >> 8) & 0xF; }
    uint32_t const_src() const { return (d1 >> 12) & 0x3FF; }
    uint32_t vec_op() const { return (d1 >> 22) & 0x1F; }
    uint32_t sca_op() const { return (d1 >> 27) & 0x1F; }
    // d3
    bool end() const { return d3 & 1; }
    bool index_const() const { return (d3 >> 1) & 1; }
    uint32_t dst() const { return (d3 >> 2) & 0x1F; }
    uint32_t sca_dst_tmp() const { return (d3 >> 7) & 0x3F; }
    bool vec_mask(int c) const { return (d3 >> (16 - c)) & 1; }  // c = 0..3 -> x..w
    bool sca_mask(int c) const { return (d3 >> (20 - c)) & 1; }
    uint32_t src(int i) const {
        if (i == 0) return ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        if (i == 1) return (d2 >> 6) & 0x1FFFF;
        return ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
    }
};

std::string src_str(const VpInsn& in, int i, uint32_t& inputs_used) {
    uint32_t s = in.src(i);
    uint32_t type = s & 3, tmp = (s >> 2) & 0x3F;
    std::string r;
    char buf[64];
    switch (type) {
        case 1: snprintf(buf, sizeof(buf), "R[%u]", tmp); r = buf; break;
        case 2:
            snprintf(buf, sizeof(buf), "i.v%u", in.input_src());
            inputs_used |= 1u << in.input_src();
            r = buf;
            break;
        case 3:
            if (in.index_const())
                snprintf(buf, sizeof(buf), "c[min(%u + A%d.%c, 467)]", in.const_src(), in.addr_reg_sel1() ? 1 : 0,
                         kSwz[in.addr_swz()]);
            else
                snprintf(buf, sizeof(buf), "c[%u]", in.const_src());
            r = buf;
            break;
        default: r = "float4(0,0,0,0)"; break;
    }
    char swz[6] = {'.', kSwz[(s >> 14) & 3], kSwz[(s >> 12) & 3], kSwz[(s >> 10) & 3], kSwz[(s >> 8) & 3], 0};
    if (strcmp(swz, ".xyzw")) r = "(" + r + ")" + swz;
    if (in.src_abs(i)) r = "abs(" + r + ")";
    if ((s >> 16) & 1) r = "(-" + r + ")";
    return r;
}

// condition test string for component c ("x".."w") against CC register
std::string cond_expr(const VpInsn& in, int c) {
    static const char* ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    uint32_t cond = in.cond();
    if (cond == 0) return "false";
    if (cond == 7) return "true";
    char buf[64];
    snprintf(buf, sizeof(buf), "(CC%d.%c %s 0.0)", in.cond_reg_sel1() ? 1 : 0, kSwz[in.cond_swz(c)], ops[cond]);
    return buf;
}

void emit_write(std::string& out, const VpInsn& in, bool sca, const std::string& value) {
    std::string mask;
    for (int c = 0; c < 4; ++c)
        if (sca ? in.sca_mask(c) : in.vec_mask(c)) mask += kSwz[c];
    if (mask.empty()) return;
    std::string v = in.saturate() ? "saturate(" + value + ")" : value;
    out += "\t{ float4 t = " + v + ";\n";
    std::vector<std::string> targets;
    uint32_t tmp = sca ? in.sca_dst_tmp() : in.dst_tmp();
    char buf[48];
    if (tmp != 0x3F) {
        snprintf(buf, sizeof(buf), "R[%u]", tmp);
        targets.push_back(buf);
    }
    if (in.dst() != 0x1F && (sca ? !in.vec_result() : in.vec_result())) {
        snprintf(buf, sizeof(buf), "o[%u]", in.dst());
        targets.push_back(buf);
    }
    for (auto& tg : targets)
        for (char m : mask) {
            int c = (int)(strchr(kSwz, m) - kSwz);
            if (in.cond_test())
                out += "\t  if (" + cond_expr(in, c) + ") " + tg + "." + m + " = t." + m + ";\n";
            else
                out += "\t  " + tg + "." + m + " = t." + m + ";\n";
        }
    if (in.cond_update0() || in.cond_update1()) {
        int cc = in.cond_reg_sel1() ? 1 : 0;
        for (char m : mask) out += std::string("\t  CC") + char('0' + cc) + "." + m + " = t." + m + ";\n";
    }
    out += "\t}\n";
}

}  // namespace

std::string vp_to_hlsl(const uint32_t* ucode, uint32_t start, uint32_t& inputs_used, uint32_t& insn_count) {
    std::string body;
    inputs_used = 0;
    insn_count = 0;
    static bool warned_branch = false;
    for (uint32_t i = start; i < 512; ++i) {
        VpInsn in{ucode[i * 4 + 0], ucode[i * 4 + 1], ucode[i * 4 + 2], ucode[i * 4 + 3]};
        ++insn_count;
        uint32_t vop = in.vec_op(), sop = in.sca_op();
        std::string s0 = src_str(in, 0, inputs_used), s1 = src_str(in, 1, inputs_used), s2 = src_str(in, 2, inputs_used);
        // ---- vector half
        std::string v;
        switch (vop) {
            case 0: break;  // NOP
            case 1: v = s0; break;
            case 2: v = s0 + " * " + s1; break;
            case 3: v = s0 + " + " + s2; break;  // ADD uses src0 and src2
            case 4: v = s0 + " * " + s1 + " + " + s2; break;
            case 5: v = "dot((" + s0 + ").xyz, (" + s1 + ").xyz).xxxx"; break;
            case 6: v = "(dot((" + s0 + ").xyz, (" + s1 + ").xyz) + (" + s1 + ").w).xxxx"; break;
            case 7: v = "dot(" + s0 + ", " + s1 + ").xxxx"; break;
            case 8: v = "float4(1.0, (" + s0 + ").y * (" + s1 + ").y, (" + s0 + ").z, (" + s1 + ").w)"; break;
            case 9: v = "min(" + s0 + ", " + s1 + ")"; break;
            case 10: v = "max(" + s0 + ", " + s1 + ")"; break;
            case 11: v = "float4((" + s0 + ") < (" + s1 + "))"; break;
            case 12: v = "float4((" + s0 + ") >= (" + s1 + "))"; break;
            case 13: {  // ARL: address register
                int a = in.addr_reg_sel1() ? 1 : 0;
                std::string mask;
                for (int c = 0; c < 4; ++c)
                    if (in.vec_mask(c)) mask += kSwz[c];
                if (!mask.empty())
                    body += "\tA" + std::to_string(a) + "." + mask + " = int4(floor(" + s0 + "))." + mask + ";\n";
                break;
            }
            case 14: v = "frac(" + s0 + ")"; break;
            case 15: v = "floor(" + s0 + ")"; break;
            case 16: v = "float4((" + s0 + ") == (" + s1 + "))"; break;
            case 17: v = "float4(0,0,0,0)"; break;
            case 18: v = "float4((" + s0 + ") > (" + s1 + "))"; break;
            case 19: v = "float4((" + s0 + ") <= (" + s1 + "))"; break;
            case 20: v = "float4((" + s0 + ") != (" + s1 + "))"; break;
            case 21: v = "float4(1,1,1,1)"; break;
            case 22: v = "sign(" + s0 + ")"; break;
            default: LOG_W("rsx", "vertex program: vector opcode %u not supported", vop); break;
        }
        if (!v.empty()) emit_write(body, in, false, v);
        // ---- scalar half (operates on src2, first swizzled component)
        std::string x = "(" + s2 + ").x";
        std::string sv;
        switch (sop) {
            case 0: break;
            case 1: sv = s2; break;
            case 2: sv = "(1.0 / " + x + ").xxxx"; break;
            case 3: sv = "(clamp(1.0 / " + x + ", -1.884467e19, 1.884467e19)).xxxx"; break;
            case 4: sv = "rsqrt(abs(" + x + ")).xxxx"; break;
            case 5: sv = "float4(exp2(floor(" + x + ")), frac(" + x + "), exp2(" + x + "), 1.0)"; break;
            case 6: sv = "float4(floor(log2(abs(" + x + "))), abs(" + x + ") / exp2(floor(log2(abs(" + x + ")))), log2(abs(" + x + ")), 1.0)"; break;
            case 7: sv = "vp_lit(" + s2 + ")"; break;
            case 13: sv = "log2(" + x + ").xxxx"; break;
            case 14: sv = "exp2(" + x + ").xxxx"; break;
            case 15: sv = "sin(" + x + ").xxxx"; break;
            case 16: sv = "cos(" + x + ").xxxx"; break;
            case 8: case 9: case 10: case 11: case 12: case 17: case 18: case 19: case 20:
                if (!warned_branch) LOG_W("rsx", "vertex program: flow control (scalar opcode %u) not supported yet", sop);
                warned_branch = true;
                break;
            default: LOG_W("rsx", "vertex program: scalar opcode %u not supported", sop); break;
        }
        if (!sv.empty()) emit_write(body, in, true, sv);
        if (in.end()) break;
    }

    std::string src =
        "cbuffer VPConsts : register(b0) { float4 c[468]; };\n"
        "cbuffer VPView : register(b1) { float4 vp_scale; float4 vp_offset; float4 rt_info; };\n"
        "struct VSIn {";
    for (int a = 0; a < 16; ++a) src += " float4 v" + std::to_string(a) + " : ATTR" + std::to_string(a) + ";";
    src += " };\nstruct VSOut { float4 pos : SV_Position;";
    for (int o = 1; o < 16; ++o) src += " float4 o" + std::to_string(o) + " : TEXCOORD" + std::to_string(o) + ";";
    src += " };\n"
           "float4 vp_lit(float4 s) {\n"
           "\tfloat4 r = float4(1.0, max(s.x, 0.0), 0.0, 1.0);\n"
           "\tif (s.x > 0.0) r.z = pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961));\n"
           "\treturn r;\n}\n"
           "VSOut main(VSIn i) {\n"
           "\tfloat4 R[48]; float4 o[16]; float4 CC0 = 0, CC1 = 0; int4 A0 = 0, A1 = 0;\n"
           "\t[unroll] for (int k = 0; k < 48; ++k) R[k] = float4(0,0,0,0);\n"
           "\t[unroll] for (int k = 0; k < 16; ++k) o[k] = float4(0,0,0,1);\n";
    src += body;
    // RSX viewport transform (window = ndc * scale + offset), then to D3D clip space.
    // rt_info: x = target width, y = target height, z = 1 if the window origin is bottom-left, w = window height
    src += "\tVSOut r;\n"
           "\tfloat4 p = o[0];\n"
           "\tfloat w = (abs(p.w) < 1e-30) ? 1e-30 : p.w;\n"
           "\tfloat3 win = p.xyz / w * vp_scale.xyz + vp_offset.xyz;\n"
           "\tif (rt_info.z != 0.0) win.y = rt_info.w - win.y;\n"
           "\tfloat2 ndc = float2(win.x / rt_info.x * 2.0 - 1.0, 1.0 - win.y / rt_info.y * 2.0);\n"
           "\tr.pos = float4(ndc * w, saturate(win.z) * w, w);\n";
    for (int o = 1; o < 16; ++o) src += "\tr.o" + std::to_string(o) + " = o[" + std::to_string(o) + "];\n";
    src += "\treturn r;\n}\n";
    return src;
}
