// Win32 window, D3D11 device, presentation and PNG screenshots.
#include "gpu.h"

#include <d3dcompiler.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../kernel/kernel.h"

Gpu g_gpu;
int g_screenshot_frame = 0;
char g_screenshot_list[256] = "";
int g_rsx_debug = 0;

namespace {

std::atomic<bool> g_window_ready{false};

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            LOG_I("gpu", "window closed - exiting");
            process_exit(0);
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

void window_thread() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DanteInfernoPort";
    RegisterClassW(&wc);
    RECT r{0, 0, (LONG)g_gpu.width, (LONG)g_gpu.height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_gpu.hwnd = CreateWindowW(wc.lpszClassName, L"Dante's Inferno (PC port - WIP)", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                               wc.hInstance, nullptr);
    ShowWindow(g_gpu.hwnd, SW_SHOW);
    g_window_ready = true;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// full-screen triangle that samples the source texture
const char* kBlitHlsl = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target { return float4(src.Sample(smp, i.uv).rgb, 1); }
)";
ID3D11VertexShader* g_blit_vs;
ID3D11PixelShader* g_blit_ps;
ID3D11SamplerState* g_blit_sampler;

// ---- minimal PNG writer (stored deflate blocks)
uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFF) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320 ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFF);
}
bool write_png(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; ++x) {
            raw.push_back(row[4 * x + 2]);  // BGRA -> RGB
            raw.push_back(row[4 * x + 1]);
            raw.push_back(row[4 * x + 0]);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

}  // namespace

bool gpu_init() {
    std::thread(window_thread).detach();
    while (!g_window_ready) std::this_thread::yield();

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_gpu.width;
    sd.BufferDesc.Height = g_gpu.height;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_gpu.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &g_gpu.swap, &g_gpu.dev, &fl, &g_gpu.ctx);
    if (FAILED(hr)) {
        LOG_E("gpu", "D3D11CreateDeviceAndSwapChain failed (0x%08lX)", (unsigned long)hr);
        return false;
    }
    g_gpu.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&g_gpu.backbuffer);
    g_gpu.dev->CreateRenderTargetView(g_gpu.backbuffer, nullptr, &g_gpu.backbuffer_rtv);

    ID3DBlob* vs = gpu_compile(kBlitHlsl, "vs", "vs_5_0");
    ID3DBlob* ps = gpu_compile(kBlitHlsl, "ps", "ps_5_0");
    if (!vs || !ps) return false;
    g_gpu.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_blit_vs);
    g_gpu.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_blit_ps);
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_gpu.dev->CreateSamplerState(&smp, &g_blit_sampler);
    LOG_I("gpu", "D3D11 device created (feature level 0x%X), window %ux%u", (unsigned)fl, g_gpu.width, g_gpu.height);
    return true;
}

ID3DBlob* gpu_compile(const std::string& src, const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOG_E("gpu", "HLSL compile failed (%s):\n%s\n--- source ---\n%s", entry,
              err ? (const char*)err->GetBufferPointer() : "?", src.c_str());
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

void gpu_present(ID3D11ShaderResourceView* src, const char* screenshot_path) {
    ID3D11DeviceContext* c = g_gpu.ctx;
    float black[4] = {0, 0, 0, 1};
    c->ClearRenderTargetView(g_gpu.backbuffer_rtv, black);
    if (src) {
        D3D11_VIEWPORT vp{0, 0, (float)g_gpu.width, (float)g_gpu.height, 0, 1};
        c->OMSetRenderTargets(1, &g_gpu.backbuffer_rtv, nullptr);
        c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        c->OMSetDepthStencilState(nullptr, 0);
        c->RSSetState(nullptr);
        c->RSSetViewports(1, &vp);
        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(g_blit_vs, nullptr, 0);
        c->PSSetShader(g_blit_ps, nullptr, 0);
        c->PSSetShaderResources(0, 1, &src);
        c->PSSetSamplers(0, 1, &g_blit_sampler);
        c->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        c->PSSetShaderResources(0, 1, &none);
    }
    if (screenshot_path) {  // before Present: DISCARD swap chains leave the back buffer undefined afterwards
        if (gpu_save_png(screenshot_path)) LOG_I("gpu", "screenshot saved to %s", screenshot_path);
        else LOG_E("gpu", "screenshot to %s failed", screenshot_path);
    }
    g_gpu.swap->Present(1, 0);
}

bool gpu_write_png(const std::string& path, const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch) {
    return write_png(path, bgra, w, h, pitch);
}

bool gpu_save_png(const std::string& path) {
    D3D11_TEXTURE2D_DESC d;
    g_gpu.backbuffer->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_gpu.dev->CreateTexture2D(&d, nullptr, &staging))) return false;
    g_gpu.ctx->CopyResource(staging, g_gpu.backbuffer);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    if (SUCCEEDED(g_gpu.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        ok = write_png(path, (const uint8_t*)m.pData, d.Width, d.Height, m.RowPitch);
        g_gpu.ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}
