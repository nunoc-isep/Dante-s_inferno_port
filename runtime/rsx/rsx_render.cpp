// RSX 3D renderer on D3D11: register file, surfaces, clears, draws, textures, shaders.
// Runs on the RSX thread (all D3D11 immediate-context calls happen here).
#include <d3d11.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "../gpu/gpu.h"
#include "../kernel/kernel.h"
#include "rsx.h"
#include "rsx_shaders.h"

namespace {

uint32_t R[0x4000];  // last value written to each method (byte offset >> 2)
inline uint32_t reg(uint32_t off) { return R[(off >> 2) & 0x3FFF]; }
inline float regf(uint32_t off) {
    float f;
    uint32_t v = reg(off);
    memcpy(&f, &v, 4);
    return f;
}

uint32_t g_vp[512 * 4];
uint32_t g_vp_load = 0, g_vp_start = 0;
float g_consts[468 * 4];
uint32_t g_const_load = 0;
bool g_ready = false;

uint32_t g_frame = 1;  // frame being built (1-based)
uint32_t g_prim = 0;
bool g_indexed = false;
std::vector<std::pair<uint32_t, uint32_t>> g_ranges;  // (first, count)

// per-frame statistics for --rsx-debug
struct FrameStats {
    uint32_t draws = 0, clears = 0, vertices = 0;
    std::string targets, clear_colors;
} g_fs;

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint64_t fnv64w(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {  // faster, 8 bytes per step
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return fnv(p + i, n - i, h);
}

uint32_t location_addr(uint32_t loc_local0_main1, uint32_t offset) {
    return loc_local0_main1 ? rsx_io_to_addr(offset) : g_rsx.local_addr + offset;
}

// ---------------------------------------------------------------- surfaces
struct Surface {
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    void release() {
        if (rtv) rtv->Release();
        if (srv) srv->Release();
        if (dsv) dsv->Release();
        if (tex) tex->Release();
        *this = Surface{};
    }
};
std::map<uint32_t, Surface> g_color, g_depth;

DXGI_FORMAT color_format(uint32_t f) {
    switch (f) {
        case 3: return DXGI_FORMAT_B5G6R5_UNORM;
        case 9: return DXGI_FORMAT_R8_UNORM;
        case 0xB: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 0xC: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 0xD: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;  // A8R8G8B8, X8R8G8B8 and friends
    }
}

Surface& get_surface(std::map<uint32_t, Surface>& map, uint32_t addr, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                     bool depth) {
    Surface& s = map[addr];
    if (s.tex && s.w == w && s.h == h && s.fmt == fmt) return s;
    s.release();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (depth) {
        d.Format = fmt;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    } else {
        d.Format = fmt;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    }
    if (FAILED(g_gpu.dev->CreateTexture2D(&d, nullptr, &s.tex))) {
        LOG_E("rsx", "cannot create %s surface %ux%u", depth ? "depth" : "color", w, h);
        return s;
    }
    if (depth) {
        g_gpu.dev->CreateDepthStencilView(s.tex, nullptr, &s.dsv);
    } else {
        g_gpu.dev->CreateRenderTargetView(s.tex, nullptr, &s.rtv);
        g_gpu.dev->CreateShaderResourceView(s.tex, nullptr, &s.srv);
        float black[4] = {0, 0, 0, 0};
        g_gpu.ctx->ClearRenderTargetView(s.rtv, black);
    }
    s.w = w;
    s.h = h;
    s.fmt = fmt;
    LOG_I("rsx", "%s surface at 0x%08X: %ux%u format %u", depth ? "depth" : "color", addr, w, h, (unsigned)fmt);
    return s;
}

struct Targets {
    Surface* color = nullptr;
    Surface* depth = nullptr;
    uint32_t w = 0, h = 0;
};

Targets bind_targets() {
    Targets t;
    uint32_t clip_w = (reg(0x0200) >> 16) + (reg(0x0200) & 0xFFFF);
    uint32_t clip_h = (reg(0x0204) >> 16) + (reg(0x0204) & 0xFFFF);
    if (!clip_w || !clip_h) return t;
    uint32_t sfmt = reg(0x0208);
    uint32_t target = reg(0x0220) & 0x1F;
    if (target) {
        bool use_b = (target & 3) == 2;
        uint32_t off = use_b ? reg(0x0218) : reg(0x0210);
        uint32_t dma = use_b ? reg(0x018C) : reg(0x0194);
        uint32_t addr = location_addr(dma & 1, off);
        t.color = &get_surface(g_color, addr, clip_w, clip_h, color_format(sfmt & 0x1F), false);
    }
    uint32_t zfmt = (sfmt >> 5) & 7;
    if (zfmt) {
        uint32_t addr = location_addr(reg(0x0198) & 1, reg(0x0214));
        t.depth = &get_surface(g_depth, addr, clip_w, clip_h,
                               zfmt == 1 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT, true);
    }
    t.w = clip_w;
    t.h = clip_h;
    ID3D11RenderTargetView* rtv = t.color ? t.color->rtv : nullptr;
    g_gpu.ctx->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, t.depth ? t.depth->dsv : nullptr);
    return t;
}

// ---------------------------------------------------------------- shaders
struct VsEntry {
    ID3D11VertexShader* vs = nullptr;
    ID3DBlob* blob = nullptr;
};
std::unordered_map<uint64_t, VsEntry> g_vs_cache;
std::unordered_map<uint64_t, ID3D11PixelShader*> g_ps_cache;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer *g_cb_consts = nullptr, *g_cb_view = nullptr;

VsEntry* get_vs() {
    // program length: scan to the end bit
    uint32_t n = 0;
    for (uint32_t i = g_vp_start; i < 512; ++i) {
        ++n;
        if (g_vp[i * 4 + 3] & 1) break;
    }
    uint64_t h = fnv(&g_vp[g_vp_start * 4], n * 16);
    auto it = g_vs_cache.find(h);
    if (it != g_vs_cache.end()) return &it->second;
    uint32_t inputs, count;
    std::string src = vp_to_hlsl(g_vp, g_vp_start, inputs, count);
    if (g_rsx_debug) {
        static int nvp = 0;
        if (FILE* f = fopen(("shader_vp_" + std::to_string(nvp++) + ".hlsl").c_str(), "w")) {
            fprintf(f, "// microcode (start %u):\n", g_vp_start);
            for (uint32_t k = 0; k < count; ++k)
                fprintf(f, "//   %08X %08X %08X %08X\n", g_vp[(g_vp_start + k) * 4], g_vp[(g_vp_start + k) * 4 + 1],
                        g_vp[(g_vp_start + k) * 4 + 2], g_vp[(g_vp_start + k) * 4 + 3]);
            fputs(src.c_str(), f);
            fclose(f);
        }
    }
    VsEntry e;
    e.blob = gpu_compile(src, "main", "vs_5_0");
    if (e.blob) g_gpu.dev->CreateVertexShader(e.blob->GetBufferPointer(), e.blob->GetBufferSize(), nullptr, &e.vs);
    LOG_I("rsx", "vertex program compiled (%u instructions, inputs 0x%X)%s", count, inputs, e.vs ? "" : " - FAILED");
    if (!g_layout && e.blob) {
        D3D11_INPUT_ELEMENT_DESC el[16];
        for (int a = 0; a < 16; ++a)
            el[a] = {"ATTR", (UINT)a, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)(16 * a), D3D11_INPUT_PER_VERTEX_DATA, 0};
        g_gpu.dev->CreateInputLayout(el, 16, e.blob->GetBufferPointer(), e.blob->GetBufferSize(), &g_layout);
    }
    return &(g_vs_cache[h] = e);
}

uint8_t tex_dim(int t) {
    uint32_t fmt = reg(0x1A04 + 0x20 * t);
    uint32_t dim = (fmt >> 4) & 0xF;
    if (fmt & 4) return 6;
    return dim == 3 ? 3 : 2;
}

ID3D11PixelShader* get_ps(uint32_t& tex_used) {
    uint32_t prog = reg(0x08E4);
    uint32_t loc = (prog & 3) == 2 ? 1 : 0;
    uint32_t addr = location_addr(loc, prog & ~3u);
    FpConfig cfg{};
    for (int t = 0; t < 16; ++t) cfg.tex_dims[t] = tex_dim(t);
    cfg.shader_control = reg(0x1D60);
    cfg.alpha_test = reg(0x0304) & 1;
    cfg.alpha_func = reg(0x0308);
    cfg.alpha_ref = (reg(0x030C) & 0xFF) / 255.0f;
    // scan the program to hash it (instructions + inline constants)
    uint32_t pc = addr;
    for (int n = 0; n < 2048; ++n) {
        uint32_t w0 = vm::read32(pc), w1 = vm::read32(pc + 4), w2 = vm::read32(pc + 8), w3 = vm::read32(pc + 12);
        auto sw = [](uint32_t v) { return (v << 16) | (v >> 16); };
        bool c = (sw(w1) & 3) == 2 || (sw(w2) & 3) == 2 || (sw(w3) & 3) == 2;
        pc += c ? 32 : 16;
        if (sw(w0) & 1) break;
    }
    uint64_t h = fnv(vm::ptr(addr), pc - addr);
    h = fnv(&cfg, sizeof(cfg), h);
    auto it = g_ps_cache.find(h);
    uint32_t size;
    if (it != g_ps_cache.end()) {
        // texture usage is cheap to recompute but we keep it with the shader
        tex_used = 0;
        for (int t = 0; t < 16; ++t)
            if (reg(0x1A0C + 0x20 * t) >> 31) tex_used |= 1u << t;
        return it->second;
    }
    std::string src = fp_to_hlsl(addr, cfg, size, tex_used);
    if (g_rsx_debug) {
        static int nfp = 0;
        char fn[64];
        snprintf(fn, sizeof(fn), "shader_fp_%d_%08X.hlsl", nfp++, addr);
        if (FILE* f = fopen(fn, "w")) {
            fputs(src.c_str(), f);
            fclose(f);
        }
    }
    ID3D11PixelShader* ps = nullptr;
    if (ID3DBlob* b = gpu_compile(src, "main", "ps_5_0")) {
        g_gpu.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
        b->Release();
    }
    LOG_I("rsx", "fragment program at 0x%08X compiled (%u bytes, textures 0x%X)%s", addr, size, tex_used,
          ps ? "" : " - FAILED");
    g_ps_cache[h] = ps;
    return ps;
}

// ---------------------------------------------------------------- textures
struct TexEntry {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
std::unordered_map<uint64_t, TexEntry> g_textures;

uint32_t swizzle_offset(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {
    uint32_t off = 0, shift = 0;
    for (uint32_t i = 0; i < 16 && (i < lw || i < lh); ++i) {
        if (i < lw) off |= ((x >> i) & 1) << shift++;
        if (i < lh) off |= ((y >> i) & 1) << shift++;
    }
    return off;
}

uint32_t log2u(uint32_t v) {
    uint32_t l = 0;
    while ((1u << l) < v) ++l;
    return l;
}

ID3D11ShaderResourceView* get_texture(int t) {
    uint32_t base = 0x1A00 + 0x20 * t;
    uint32_t offset = reg(base), fmtw = reg(base + 4), rect = reg(base + 0x18);
    uint32_t pitch = reg(0x1840 + 4 * t) & 0xFFFFF;
    uint32_t w = rect >> 16, h = rect & 0xFFFF;
    if (!w || !h) return nullptr;
    uint32_t loc = (fmtw & 3) == 2 ? 1 : 0;
    uint32_t addr = location_addr(loc, offset);
    // render-to-texture: the pixels of a colour surface live on the GPU, not in guest memory
    auto rt = g_color.find(addr);
    if (rt != g_color.end() && rt->second.srv) return rt->second.srv;
    uint32_t format = (fmtw >> 8) & 0xFF;
    bool linear = (format & 0x20) != 0;
    uint32_t f = format & ~0x60u;
    uint32_t bpp = 4;
    DXGI_FORMAT dfmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool compressed = false;
    switch (f) {
        case 0x81: bpp = 1; break;
        case 0x82: case 0x83: case 0x84: case 0x8B: bpp = 2; break;
        case 0x86: compressed = true; dfmt = DXGI_FORMAT_BC1_UNORM; bpp = 8; break;
        case 0x87: compressed = true; dfmt = DXGI_FORMAT_BC2_UNORM; bpp = 16; break;
        case 0x88: compressed = true; dfmt = DXGI_FORMAT_BC3_UNORM; bpp = 16; break;
        case 0x85: case 0x9E: case 0x8F: bpp = 4; break;
        default:
            static uint32_t warned[256];
            if (!warned[f]++) LOG_W("rsx", "texture format 0x%02X not supported yet (unit %d)", f, t);
            bpp = 4;
            break;
    }
    uint32_t bw = compressed ? (w + 3) / 4 : w, bh = compressed ? (h + 3) / 4 : h;
    if (!pitch || !linear) pitch = bw * bpp;
    if (compressed) pitch = std::max(pitch, bw * bpp);
    size_t bytes = (size_t)pitch * bh;
    if (!vm::ptr(addr) || addr + bytes > 0xF0000000ull) return nullptr;
    uint64_t key = ((uint64_t)addr << 32) ^ ((uint64_t)fmtw << 8) ^ rect ^ ((uint64_t)pitch << 40);
    uint64_t hash = fnv64w(vm::ptr(addr), bytes);
    TexEntry& e = g_textures[key];
    if (e.tex && e.hash == hash) return e.srv;

    // convert to the host layout
    std::vector<uint8_t> out;
    uint32_t out_pitch;
    const uint8_t* src = vm::ptr(addr);
    if (compressed) {
        out.assign(src, src + bytes);  // DXT blocks are stored in the standard layout
        out_pitch = pitch;
    } else {
        out.resize((size_t)w * h * 4);
        out_pitch = w * 4;
        uint32_t lw = log2u(w), lh = log2u(h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                size_t so = linear ? (size_t)y * pitch + (size_t)x * bpp : (size_t)swizzle_offset(x, y, lw, lh) * bpp;
                const uint8_t* p = src + so;
                uint8_t* d = &out[((size_t)y * w + x) * 4];  // B G R A
                uint8_t a = 255, r = 0, g = 0, b = 0;
                switch (f) {
                    case 0x81: a = r = g = b = p[0]; break;
                    case 0x82: { uint16_t v = (p[0] << 8) | p[1]; a = (v >> 15) ? 255 : 0; r = ((v >> 10) & 31) * 255 / 31; g = ((v >> 5) & 31) * 255 / 31; b = (v & 31) * 255 / 31; break; }
                    case 0x83: { uint16_t v = (p[0] << 8) | p[1]; a = ((v >> 12) & 15) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; break; }
                    case 0x84: { uint16_t v = (p[0] << 8) | p[1]; r = ((v >> 11) & 31) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; break; }
                    case 0x8B: g = p[0]; b = p[1]; break;
                    case 0x9E: r = p[1]; g = p[2]; b = p[3]; break;
                    default: a = p[0]; r = p[1]; g = p[2]; b = p[3]; break;  // A8R8G8B8
                }
                d[0] = b;
                d[1] = g;
                d[2] = r;
                d[3] = a;
            }
    }
    if (!e.tex || e.w != w || e.h != h || e.fmt != dfmt) {
        if (e.srv) e.srv->Release();
        if (e.tex) e.tex->Release();
        e = TexEntry{};
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = dfmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_gpu.dev->CreateTexture2D(&d, nullptr, &e.tex))) return nullptr;
        g_gpu.dev->CreateShaderResourceView(e.tex, nullptr, &e.srv);
        e.w = w;
        e.h = h;
        e.fmt = dfmt;
    }
    g_gpu.ctx->UpdateSubresource(e.tex, 0, nullptr, out.data(), out_pitch, 0);
    e.hash = hash;
    return e.srv;
}

D3D11_TEXTURE_ADDRESS_MODE wrap_mode(uint32_t m) {
    switch (m) {
        case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 4: case 7: return D3D11_TEXTURE_ADDRESS_BORDER;
        case 6: case 8: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

ID3D11SamplerState* get_sampler(int t) {
    uint32_t base = 0x1A00 + 0x20 * t;
    uint32_t addr = reg(base + 8), filter = reg(base + 0x14);
    D3D11_SAMPLER_DESC d{};
    bool min_lin = ((filter >> 16) & 0xF) != 1 && ((filter >> 16) & 0xF) != 3 && ((filter >> 16) & 0xF) != 5;
    bool mag_lin = ((filter >> 24) & 0xF) != 1;
    d.Filter = min_lin ? (mag_lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT)
                       : (mag_lin ? D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT);
    d.AddressU = wrap_mode(addr & 0xF);
    d.AddressV = wrap_mode((addr >> 8) & 0xF);
    d.AddressW = wrap_mode((addr >> 16) & 0xF);
    d.MaxLOD = 0;  // only level 0 is uploaded for now
    ID3D11SamplerState* s = nullptr;
    g_gpu.dev->CreateSamplerState(&d, &s);  // D3D11 de-duplicates identical state objects
    return s;
}

// ---------------------------------------------------------------- render state
D3D11_BLEND blend_factor(uint32_t f, bool alpha) {
    switch (f) {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D11_BLEND_SRC_ALPHA;
        case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D11_BLEND_DEST_ALPHA;
        case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
        case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
        default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND_OP blend_op(uint32_t e) {
    switch (e) {
        case 0x8007: return D3D11_BLEND_OP_MIN;
        case 0x8008: return D3D11_BLEND_OP_MAX;
        case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
        default: return D3D11_BLEND_OP_ADD;
    }
}
D3D11_COMPARISON_FUNC cmp_func(uint32_t f) { return (D3D11_COMPARISON_FUNC)((f & 7) + 1); }

void apply_state(const Targets& t) {
    ID3D11DeviceContext* c = g_gpu.ctx;
    // blend
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = reg(0x0310) & 1;
    uint32_t sf = reg(0x0314), df = reg(0x0318), eq = reg(0x0320);
    rt.SrcBlend = blend_factor(sf & 0xFFFF, false);
    rt.DestBlend = blend_factor(df & 0xFFFF, false);
    rt.BlendOp = blend_op(eq & 0xFFFF);
    rt.SrcBlendAlpha = blend_factor(sf >> 16, true);
    rt.DestBlendAlpha = blend_factor(df >> 16, true);
    rt.BlendOpAlpha = blend_op(eq >> 16);
    uint32_t cm = reg(0x0324);
    rt.RenderTargetWriteMask = ((cm >> 16) & 1 ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
                               ((cm >> 8) & 1 ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                               (cm & 1 ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
                               ((cm >> 24) & 1 ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    ID3D11BlendState* bs = nullptr;
    g_gpu.dev->CreateBlendState(&bd, &bs);
    uint32_t bc = reg(0x031C);
    float bf[4] = {((bc >> 16) & 0xFF) / 255.f, ((bc >> 8) & 0xFF) / 255.f, (bc & 0xFF) / 255.f, (bc >> 24) / 255.f};
    c->OMSetBlendState(bs, bf, 0xFFFFFFFF);
    if (bs) bs->Release();
    // depth
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = t.depth && (reg(0x0A74) & 1);
    dd.DepthWriteMask = (reg(0x0A70) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = cmp_func(reg(0x0A6C));
    ID3D11DepthStencilState* ds = nullptr;
    g_gpu.dev->CreateDepthStencilState(&dd, &ds);
    c->OMSetDepthStencilState(ds, 0);
    if (ds) ds->Release();
    // raster: culling stays off until window-origin winding is verified
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr;
    g_gpu.dev->CreateRasterizerState(&rd, &rs);
    c->RSSetState(rs);
    if (rs) rs->Release();
    D3D11_VIEWPORT vp{0, 0, (float)t.w, (float)t.h, 0, 1};
    c->RSSetViewports(1, &vp);
    uint32_t sx = reg(0x08C0) & 0xFFFF, sw = reg(0x08C0) >> 16, sy = reg(0x08C4) & 0xFFFF, sh = reg(0x08C4) >> 16;
    uint32_t win = reg(0x1D88);
    bool bottom = ((win >> 12) & 0xF) == 1;
    if (bottom) sy = (win & 0xFFF) > sy + sh ? (win & 0xFFF) - (sy + sh) : 0;
    D3D11_RECT sc{(LONG)sx, (LONG)sy, (LONG)(sx + sw), (LONG)(sy + sh)};
    if (!sw || !sh) sc = {0, 0, (LONG)t.w, (LONG)t.h};
    c->RSSetScissorRects(1, &sc);
}

// ---------------------------------------------------------------- vertex fetch
float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? INFINITY : std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

void fetch_attr(int a, uint32_t index, float out[4]) {
    uint32_t fmt = reg(0x1740 + 4 * a);
    uint32_t type = fmt & 0xF, size = (fmt >> 4) & 0xF, stride = (fmt >> 8) & 0xFF;
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    if (!size) {  // constant attribute (NV4097_SET_VERTEX_DATA4F_M)
        for (int k = 0; k < 4; ++k) out[k] = regf(0x1C00 + 16 * a + 4 * k);
        return;
    }
    uint32_t off = reg(0x1680 + 4 * a);
    uint32_t base = location_addr(off >> 31, (off & 0x7FFFFFFF) + reg(0x173C));
    uint32_t p = base + stride * index;
    for (uint32_t k = 0; k < size && k < 4; ++k) {
        switch (type) {
            case 1: out[k] = std::max(-1.0f, (int16_t)vm::read16(p + 2 * k) / 32767.0f); break;
            case 2: { uint32_t v = vm::read32(p + 4 * k); memcpy(&out[k], &v, 4); break; }
            case 3: out[k] = half_to_float(vm::read16(p + 2 * k)); break;
            case 4: out[k] = vm::read8(p + k) / 255.0f; break;
            case 5: out[k] = (float)(int16_t)vm::read16(p + 2 * k); break;
            case 7: out[k] = (float)vm::read8(p + k); break;
            case 6: {  // CMP: 11:11:10 signed normalised, single dword
                uint32_t v = vm::read32(p);
                int32_t x = (int32_t)(v << 21) >> 21, y = (int32_t)(v << 10) >> 21, z = (int32_t)v >> 22;
                out[0] = x / 1023.0f;
                out[1] = y / 1023.0f;
                out[2] = z / 511.0f;
                k = 4;
                break;
            }
            default: break;
        }
    }
}

void draw() {
    if (g_ranges.empty()) return;
    Targets t = bind_targets();
    if (!t.color && !t.depth) return;
    VsEntry* vs = get_vs();
    uint32_t tex_used = 0;
    ID3D11PixelShader* ps = get_ps(tex_used);
    if (!vs || !vs->vs || !ps || !g_layout) return;

    // gather vertex ids
    std::vector<uint32_t> ids;
    uint32_t idx_off = reg(0x181C), idx_dma = reg(0x1820);
    uint32_t idx_addr = location_addr(idx_dma & 0xF, idx_off);
    bool idx16 = ((idx_dma >> 4) & 0xF) == 1;
    uint32_t base_index = reg(0x1738);
    for (auto& [first, count] : g_ranges)
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t i = first + k;
            ids.push_back(g_indexed ? (idx16 ? vm::read16(idx_addr + 2 * i) : vm::read32(idx_addr + 4 * i)) + base_index : i);
        }
    uint32_t n = (uint32_t)ids.size();
    std::vector<float> verts((size_t)n * 64);
    for (uint32_t v = 0; v < n; ++v)
        for (int a = 0; a < 16; ++a) fetch_attr(a, ids[v], &verts[(size_t)v * 64 + a * 4]);

    // topology -> index list
    std::vector<uint32_t> ix;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (g_prim) {
        case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; for (uint32_t i = 0; i < n; ++i) ix.push_back(i); break;
        case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; for (uint32_t i = 0; i < n; ++i) ix.push_back(i); break;
        case 3: case 4:
            topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
            for (uint32_t i = 0; i < n; ++i) ix.push_back(i);
            if (g_prim == 3 && n) ix.push_back(0);
            break;
        case 5: for (uint32_t i = 0; i + 2 < n; i += 3) ix.insert(ix.end(), {i, i + 1, i + 2}); break;
        case 6: for (uint32_t i = 0; i + 2 < n; ++i) (i & 1) ? ix.insert(ix.end(), {i + 1, i, i + 2}) : ix.insert(ix.end(), {i, i + 1, i + 2}); break;
        case 7: case 10: for (uint32_t i = 1; i + 1 < n; ++i) ix.insert(ix.end(), {0, i, i + 1}); break;
        case 8: for (uint32_t q = 0; q + 3 < n; q += 4) ix.insert(ix.end(), {q, q + 1, q + 2, q, q + 2, q + 3}); break;
        case 9: for (uint32_t i = 0; i + 3 < n; i += 2) ix.insert(ix.end(), {i, i + 1, i + 2, i + 1, i + 3, i + 2}); break;
        default: return;
    }
    if (ix.empty()) return;

    ID3D11DeviceContext* c = g_gpu.ctx;
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = (UINT)(verts.size() * 4);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{verts.data(), 0, 0};
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    g_gpu.dev->CreateBuffer(&bd, &sd, &vb);
    bd.ByteWidth = (UINT)(ix.size() * 4);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = ix.data();
    g_gpu.dev->CreateBuffer(&bd, &sd, &ib);
    UINT stride = 16 * 16, offset = 0;  // 16 float4 attributes per vertex
    c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    c->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
    c->IASetInputLayout(g_layout);
    c->IASetPrimitiveTopology(topo);

    // constants
    c->UpdateSubresource(g_cb_consts, 0, nullptr, g_consts, 0, 0);
    // VPView: scale.xyzw, offset.xyzw, rt_info = (target width, target height, bottom-left origin?, window height)
    uint32_t win = reg(0x1D88);
    bool bottom = ((win >> 12) & 0xF) == 1;
    float view_cb[12] = {regf(0x0A30), regf(0x0A34), regf(0x0A38), regf(0x0A3C),
                         regf(0x0A20), regf(0x0A24), regf(0x0A28), regf(0x0A2C),
                         (float)t.w, (float)t.h, bottom ? 1.0f : 0.0f, (win & 0xFFF) ? (float)(win & 0xFFF) : (float)t.h};
    c->UpdateSubresource(g_cb_view, 0, nullptr, view_cb, 0, 0);
    ID3D11Buffer* cbs[2] = {g_cb_consts, g_cb_view};
    c->VSSetConstantBuffers(0, 2, cbs);
    c->VSSetShader(vs->vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);

    // textures
    ID3D11ShaderResourceView* srvs[16] = {};
    ID3D11SamplerState* smps[16] = {};
    for (int u = 0; u < 16; ++u) {
        if (!(tex_used & (1u << u))) continue;
        srvs[u] = get_texture(u);
        smps[u] = get_sampler(u);
    }
    c->PSSetShaderResources(0, 16, srvs);
    c->PSSetSamplers(0, 16, smps);
    for (auto* s : smps)
        if (s) s->Release();

    apply_state(t);
    ++g_fs.draws;
    g_fs.vertices += n;
    {
        char b[48];
        uint32_t ta = 0;
        for (auto& kv : g_color)
            if (&kv.second == t.color) ta = kv.first;
        snprintf(b, sizeof(b), " %08X:%u", ta, n);
        if (g_fs.targets.size() < 400) g_fs.targets += b;
    }
    static int dbg = 0;
    if (g_rsx_debug && (uint32_t)g_rsx_debug == g_frame) {
        ++dbg;
        const float* v0 = &verts[0];
        LOG_I("rsxdbg", "draw %d: prim %u, %u vertices (%s), target %ux%u, vp start %u, fp 0x%08X, tex 0x%X", dbg,
              g_prim, n, g_indexed ? "indexed" : "arrays", t.w, t.h, g_vp_start, reg(0x08E4), tex_used);
        for (int a = 0; a < 16; ++a)
            if (reg(0x1740 + 4 * a) & 0xF0)
                LOG_I("rsxdbg", "  attr %d fmt 0x%08X off 0x%08X v0 = (%g, %g, %g, %g)", a, reg(0x1740 + 4 * a),
                      reg(0x1680 + 4 * a), v0[a * 4], v0[a * 4 + 1], v0[a * 4 + 2], v0[a * 4 + 3]);
        LOG_I("rsxdbg", "  viewport scale (%g %g %g %g) offset (%g %g %g %g), window 0x%X, clip %08X %08X, scissor %08X %08X",
              view_cb[0], view_cb[1], view_cb[2], view_cb[3], view_cb[4], view_cb[5], view_cb[6], view_cb[7], win,
              reg(0x0200), reg(0x0204), reg(0x08C0), reg(0x08C4));
        LOG_I("rsxdbg", "  blend %u (0x%X/0x%X) colormask 0x%08X depth test %u func 0x%X alpha test %u, c467=(%g %g %g %g), shader control 0x%X",
              reg(0x0310), reg(0x0314), reg(0x0318), reg(0x0324), reg(0x0A74), reg(0x0A6C), reg(0x0304), g_consts[467 * 4],
              g_consts[467 * 4 + 1], g_consts[467 * 4 + 2], g_consts[467 * 4 + 3], reg(0x1D60));
        for (int u = 0; u < 16; ++u)
            if (tex_used & (1u << u))
            {
                uint32_t fw = reg(0x1A04 + 0x20 * u), rc = reg(0x1A18 + 0x20 * u);
                uint32_t ta = location_addr((fw & 3) == 2 ? 1 : 0, reg(0x1A00 + 0x20 * u));
                uint64_t sum = 0, cnt = std::min<uint64_t>((uint64_t)(rc >> 16) * (rc & 0xFFFF), 1u << 20);
                for (uint64_t k = 0; k < cnt; ++k) sum += vm::read8(ta + (uint32_t)k);
                if (((fw >> 8) & 0xDF) == 0x81 && !g_color.count(ta)) {  // B8 plane: save as greyscale PNG
                    uint32_t tw = rc >> 16, th = rc & 0xFFFF, tp = reg(0x1840 + 4 * u) & 0xFFFFF;
                    std::vector<uint8_t> img((size_t)tw * th * 4);
                    for (uint32_t y = 0; y < th; ++y)
                        for (uint32_t x = 0; x < tw; ++x) {
                            uint8_t v = vm::read8(ta + y * (tp ? tp : tw) + x);
                            uint8_t* d = &img[((size_t)y * tw + x) * 4];
                            d[0] = d[1] = d[2] = v;
                            d[3] = 255;
                        }
                    gpu_write_png("tex_frame" + std::to_string(g_frame) + "_unit" + std::to_string(u) + ".png", img.data(), tw, th, tw * 4);
                }
                LOG_I("rsxdbg", "  tex %d: offset 0x%08X format 0x%08X rect 0x%08X control0 0x%08X pitch %u, avg byte %.1f%s", u,
                      reg(0x1A00 + 0x20 * u), fw, rc, reg(0x1A0C + 0x20 * u), reg(0x1840 + 4 * u) & 0xFFFFF,
                      cnt ? (double)sum / cnt : 0.0, g_color.count(ta) ? " (render target)" : "");
            }
    }
    c->DrawIndexed((UINT)ix.size(), 0, 0);
    vb->Release();
    ib->Release();
    ID3D11ShaderResourceView* none[16] = {};
    c->PSSetShaderResources(0, 16, none);
}

void clear(uint32_t mask) {
    Targets t = bind_targets();
    ++g_fs.clears;
    {
        char b[48];
        snprintf(b, sizeof(b), " %X:%08X", mask, reg(0x1D90));
        if (g_fs.clear_colors.size() < 200) g_fs.clear_colors += b;
    }
    if (t.color && (mask & 0xF0)) {
        uint32_t v = reg(0x1D90);
        float col[4] = {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f, (v >> 24) / 255.f};
        g_gpu.ctx->ClearRenderTargetView(t.color->rtv, col);
    }
    if (t.depth && (mask & 3)) {
        uint32_t z = reg(0x1D8C);
        bool z16 = t.depth->fmt == DXGI_FORMAT_D16_UNORM;
        float depth = z16 ? (z & 0xFFFF) / 65535.0f : (z >> 8) / 16777215.0f;
        UINT flags = ((mask & 1) ? D3D11_CLEAR_DEPTH : 0) | ((mask & 2) ? D3D11_CLEAR_STENCIL : 0);
        g_gpu.ctx->ClearDepthStencilView(t.depth->dsv, flags, depth, (UINT8)(z & 0xFF));
    }
}

}  // namespace

bool rsx_render_init() {
    if (!gpu_init()) return false;
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof(g_consts);
    g_gpu.dev->CreateBuffer(&bd, nullptr, &g_cb_consts);
    bd.ByteWidth = 48;
    g_gpu.dev->CreateBuffer(&bd, nullptr, &g_cb_view);
    g_ready = true;
    return true;
}

void rsx_render_method(uint32_t m, uint32_t arg) {
    R[(m >> 2) & 0x3FFF] = arg;
    if (!g_ready) return;
    bool dbg = g_rsx_debug && (uint32_t)g_rsx_debug == g_frame;
    if (m >= 0x0B80 && m < 0x0C00) {
        if (dbg && (g_vp_load & 3) == 0) LOG_I("rsxdbg", "  vp upload slot %u", g_vp_load / 4);
        g_vp[g_vp_load++ & 2047] = arg;
        return;
    }
    if (m >= 0x1F00 && m < 0x1F80) {
        uint32_t i = g_const_load++;
        if (dbg) {
            float f;
            memcpy(&f, &arg, 4);
            LOG_I("rsxdbg", "  const c[%u].%c = %g (method 0x%04X)", i / 4, "xyzw"[i & 3], f, m);
        }
        if (i < 468 * 4) memcpy(&g_consts[i], &arg, 4);
        return;
    }
    if (dbg && m == 0x1EFC) LOG_I("rsxdbg", "  const load index %u", arg);
    if (dbg && m == 0x1808) LOG_I("rsxdbg", "  begin/end %u", arg);
    switch (m) {
        case 0x1E9C: g_vp_load = arg * 4; break;
        case 0x1EA0: g_vp_start = arg & 511; break;
        case 0x1EFC: g_const_load = arg * 4; break;
        case 0x1D94: clear(arg); break;
        case 0x1808:
            if (arg) {
                g_prim = arg;
                g_ranges.clear();
            } else {
                draw();
                g_ranges.clear();
            }
            break;
        case 0x1814: g_indexed = false; g_ranges.push_back({arg & 0xFFFFFF, (arg >> 24) + 1}); break;
        case 0x1824: g_indexed = true; g_ranges.push_back({arg & 0xFFFFFF, (arg >> 24) + 1}); break;
        default: break;
    }
}

void rsx_render_flip(uint32_t buffer) {
    if (!g_ready) return;
    auto& d = g_rsx.displays[buffer & 7];
    uint32_t addr = g_rsx.local_addr + d.offset;
    ID3D11ShaderResourceView* srv = nullptr;
    auto it = g_color.find(addr);
    if (it != g_color.end()) srv = it->second.srv;
    uint32_t frame = g_frame++;
    if (g_rsx_debug && (frame <= 5 || frame % 300 == 0))
        LOG_I("rsxdbg", "frame %u: %u draws (%u vertices), %u clears [%s], draws by target [%s]", frame, g_fs.draws,
              g_fs.vertices, g_fs.clears, g_fs.clear_colors.c_str(), g_fs.targets.c_str());
    g_fs = FrameStats{};
    char path[64];
    char key[16];
    snprintf(key, sizeof(key), ",%u,", frame);
    bool shot = g_screenshot_frame && strstr(g_screenshot_list, key);
    if (shot) snprintf(path, sizeof(path), "screenshot_%u.png", frame);
    gpu_present(srv, shot ? path : nullptr);
}
