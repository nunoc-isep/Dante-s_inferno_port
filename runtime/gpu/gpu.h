// Host GPU layer: Win32 window + D3D11 device/swap chain.
// All D3D11 calls are made from the RSX thread; the window runs its own message loop.
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>

struct Gpu {
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* backbuffer_rtv = nullptr;
    ID3D11Texture2D* backbuffer = nullptr;
    uint32_t width = 1280, height = 720;
};
extern Gpu g_gpu;

// Create the window (own thread) and the D3D11 device. Call from the RSX thread.
bool gpu_init();
// Copy `src` to the back buffer (scaled) and present; saves a PNG first when `screenshot_path` is set.
void gpu_present(ID3D11ShaderResourceView* src, const char* screenshot_path = nullptr);
// Compile HLSL source; returns nullptr and logs errors on failure.
ID3DBlob* gpu_compile(const std::string& src, const char* entry, const char* target);
// Save the current back buffer as a PNG (used by --screenshot).
bool gpu_save_png(const std::string& path);
bool gpu_write_png(const std::string& path, const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t pitch);

extern int g_screenshot_frame;  // --screenshot N[,M...]: save these frames to screenshot_N.png (0 = off)
extern char g_screenshot_list[256];
extern int g_rsx_debug;  // --rsx-debug N: log details of the first N draws and dump shader sources
