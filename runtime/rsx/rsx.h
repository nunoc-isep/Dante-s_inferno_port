// Minimal RSX (PS3 GPU) front end: command-buffer processing and sync.
// No rendering yet - draw methods are consumed and ignored.
#pragma once

#include <atomic>
#include <cstdint>

struct RsxState {
    // guest addresses
    uint32_t local_addr = 0xC0000000, local_size = 0x0F900000;
    uint32_t io_addr = 0, io_size = 0;  // main memory mapped at io offset 0
    uint32_t control_addr = 0;          // CellGcmControl {put, get, ref}
    uint32_t label_addr = 0;            // 256 labels x 16 bytes
    uint32_t context_addr = 0;          // default CellGcmContextData
    uint32_t context_ptr_addr = 0;      // where the game keeps its context pointer
    uint32_t flip_mode = 0;
    uint32_t user_handler = 0, flip_handler = 0, vblank_handler = 0;
    struct Display {
        uint32_t offset, pitch, width, height;
    } displays[8]{};
    std::atomic<uint32_t> flips{0};
    std::atomic<bool> running{false};
};
extern RsxState g_rsx;

void rsx_start();
// Translate an RSX offset (io space) to a guest address; 0 if unmapped.
uint32_t rsx_io_to_addr(uint32_t offset);
