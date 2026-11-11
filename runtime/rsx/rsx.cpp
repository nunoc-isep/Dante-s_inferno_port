// RSX command processor thread (FIFO walker). Implements the control-flow
// commands and the sync methods; everything else is skipped for now.
#include "rsx.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../kernel/kernel.h"

void host_sleep_us(uint64_t us);  // ppc_runtime.cpp: accurate short sleeps

RsxState g_rsx;

uint32_t rsx_io_to_addr(uint32_t offset) {
    if (offset < g_rsx.io_size) return g_rsx.io_addr + offset;
    return 0;
}

// method offsets (bytes)
enum : uint32_t {
    NV406E_SET_REFERENCE = 0x0050,
    NV406E_SEMAPHORE_OFFSET = 0x0064,
    NV406E_SEMAPHORE_ACQUIRE = 0x0068,
    NV406E_SEMAPHORE_RELEASE = 0x006C,
    NV4097_SET_SEMAPHORE_OFFSET = 0x1D6C,
    NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1D70,
    NV4097_TEXTURE_READ_SEMAPHORE_RELEASE = 0x1D74,
    GCM_FLIP_COMMAND = 0xFEAC,
};

// diagnostics: per-method counts, printed once after 600 flips
static uint32_t g_method_count[0x4000];
static void dump_method_histogram() {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (uint32_t i = 0; i < 0x4000; ++i)
        if (g_method_count[i]) v.push_back({g_method_count[i], i * 4});
    std::sort(v.rbegin(), v.rend());
    std::string out;
    char buf[48];
    for (size_t k = 0; k < v.size() && k < 80; ++k) {
        snprintf(buf, sizeof(buf), " %04X:%u", v[k].second, v[k].first);
        out += buf;
    }
    LOG_I("rsx", "method histogram (offset:count, %zu distinct):%s", v.size(), out.c_str());
}

static void rsx_thread() {
    uint32_t ctrl = g_rsx.control_addr;
    uint32_t sem_offset_406e = 0, sem_offset_4097 = 0;
    std::vector<uint32_t> call_stack;
    uint64_t methods = 0;
    LOG_I("rsx", "command processor started (control 0x%08X, io 0x%08X+0x%X)", ctrl, g_rsx.io_addr, g_rsx.io_size);
    if (!rsx_render_init()) LOG_E("rsx", "renderer unavailable - commands are processed without drawing");
    while (g_rsx.running) {
        uint32_t put = vm::read32(ctrl + 0), get = vm::read32(ctrl + 4);
        if (put == get) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        uint32_t addr = rsx_io_to_addr(get);
        if (!addr) {
            LOG_E("rsx", "get offset 0x%08X outside io space, stopping", get);
            g_rsx.running = false;
            break;
        }
        uint32_t cmd = vm::read32(addr);
        if ((cmd & 0xE0000003) == 0x20000000) {  // old-style jump
            vm::write32(ctrl + 4, cmd & 0x1FFFFFFC);
            continue;
        }
        if ((cmd & 3) == 1) {  // jump
            vm::write32(ctrl + 4, cmd & 0xFFFFFFFC);
            continue;
        }
        if ((cmd & 3) == 2) {  // call
            call_stack.push_back(get + 4);
            vm::write32(ctrl + 4, cmd & 0xFFFFFFFC);
            continue;
        }
        if (cmd == 0x00020000) {  // return
            if (call_stack.empty()) {
                LOG_E("rsx", "return with empty call stack at 0x%08X", get);
                vm::write32(ctrl + 4, get + 4);
            } else {
                vm::write32(ctrl + 4, call_stack.back());
                call_stack.pop_back();
            }
            continue;
        }
        uint32_t count = (cmd >> 18) & 0x7FF;
        uint32_t method = cmd & 0xFFFC;
        bool non_inc = (cmd & 0x40000000) != 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t m = non_inc ? method : method + 4 * i;
            uint32_t arg = vm::read32(addr + 4 + 4 * i);
            ++methods;
            ++g_method_count[(m >> 2) & 0x3FFF];
            rsx_render_method(m, arg);
            switch (m) {
                case NV406E_SET_REFERENCE: vm::write32(ctrl + 8, arg); break;
                case NV406E_SEMAPHORE_OFFSET: sem_offset_406e = arg; break;
                case NV406E_SEMAPHORE_ACQUIRE: {
                    uint32_t la = g_rsx.label_addr + sem_offset_406e;
                    auto t0 = std::chrono::steady_clock::now();
                    while (g_rsx.running && vm::read32(la) != arg) {
                        std::this_thread::sleep_for(std::chrono::microseconds(20));
                        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
                            LOG_W("rsx", "semaphore acquire timeout (label +0x%X want 0x%X have 0x%X)", sem_offset_406e,
                                  arg, vm::read32(la));
                            break;
                        }
                    }
                    break;
                }
                case NV406E_SEMAPHORE_RELEASE: vm::write32(g_rsx.label_addr + sem_offset_406e, arg); break;
                case NV4097_SET_SEMAPHORE_OFFSET: sem_offset_4097 = arg; break;
                case NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE:
                    vm::write32(g_rsx.label_addr + sem_offset_4097,
                                (arg & 0xFF00FF00) | ((arg & 0xFF) << 16) | ((arg >> 16) & 0xFF));
                    break;
                case NV4097_TEXTURE_READ_SEMAPHORE_RELEASE: vm::write32(g_rsx.label_addr + sem_offset_4097, arg); break;
                case GCM_FLIP_COMMAND: {
                    // the PS3 flips on a 59.94 Hz vblank: never show frames faster than that, even
                    // on a high-refresh monitor (game timing may assume it)
                    static auto next_vblank = std::chrono::steady_clock::now();
                    constexpr auto kFrame = std::chrono::microseconds(16683);
                    auto now = std::chrono::steady_clock::now();
                    if (now < next_vblank)
                        host_sleep_us((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(next_vblank - now).count());
                    next_vblank = std::max(next_vblank + kFrame, now);  // resync after a slow frame
                    uint32_t n = ++g_rsx.flips;
                    if (n == 600) dump_method_histogram();
                    rsx_render_flip(arg);
                    if (n <= 3 || n % 300 == 0) LOG_I("rsx", "flip #%u (buffer %u), %llu methods so far", n, arg,
                                                      (unsigned long long)methods);
                    break;
                }
                default: break;
            }
        }
        vm::write32(ctrl + 4, get + 4 + 4 * count);
    }
}

void rsx_start() {
    if (g_rsx.running.exchange(true)) return;
    std::thread(rsx_thread).detach();
}
