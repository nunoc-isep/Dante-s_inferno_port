// cellGcmSys: RSX initialisation, memory mapping, command buffer, display/flip.
#include "../kernel/kernel.h"
#include "../kernel/sync.h"
#include "../rsx/rsx.h"

#include <thread>

enum : uint32_t { CELL_GCM_ERROR_FAILURE = 0x802100FF, CELL_GCM_ERROR_INVALID_VALUE = 0x80210002 };

// CellGcmContextData: u32 begin, end, current, callback (OPD)
// Command-buffer-full callback: hand everything written so far to the RSX,
// wait for it to drain, then wrap back to the start of the buffer.
static PPC_FUNC(gcm_context_callback) {
    uint32_t c = ARG32(0);
    uint32_t begin = vm::read32(c + 0), current = vm::read32(c + 8);
    uint32_t ctrl = g_rsx.control_addr;
    uint32_t begin_off = begin - g_rsx.io_addr;
    // jump back to the start, publish, wait until the RSX has consumed it all
    vm::write32(current, 0x20000000 | begin_off);
    vm::write32(ctrl + 0, current - g_rsx.io_addr);
    while (g_rsx.running) {
        uint32_t get = vm::read32(ctrl + 4);
        if (get == begin_off) break;
        std::this_thread::yield();
    }
    vm::write32(c + 8, begin);
    vm::write32(ctrl + 0, begin_off);
    RET(CELL_OK);
}

// _cellGcmInitBody(context**, cmdSize, ioSize, ioAddress)
HLE_FUNC(cellGcmSys, _cellGcmInitBody) {
    uint32_t ctx_pp = ARG32(0), cmd_size = ARG32(1), io_size = ARG32(2), io_addr = ARG32(3);
    LOG_I("gcm", "_cellGcmInitBody(cmdSize 0x%X, ioSize 0x%X, ioAddress 0x%08X)", cmd_size, io_size, io_addr);
    if (!vm::alloc_fixed(vm::Area::Video, g_rsx.local_addr, g_rsx.local_size)) LOG_W("gcm", "local memory already mapped");
    g_rsx.io_addr = io_addr;
    g_rsx.io_size = io_size;
    uint32_t sys = vm::alloc(vm::Area::Kernel, 0x3000, 0x1000);
    g_rsx.control_addr = sys + 0x40;
    g_rsx.context_addr = sys + 0x100;
    g_rsx.label_addr = sys + 0x1000;
    memset(vm::ptr(sys), 0, 0x3000);
    // default context: the first 4 KB of the io area are reserved (as in the SDK)
    uint32_t c = g_rsx.context_addr;
    vm::write32(c + 0, io_addr + 0x1000);
    vm::write32(c + 4, io_addr + cmd_size - 4);
    vm::write32(c + 8, io_addr + 0x1000);
    vm::write32(c + 12, hle_make_callback(gcm_context_callback));
    vm::write32(g_rsx.control_addr + 0, 0x1000);  // put
    vm::write32(g_rsx.control_addr + 4, 0x1000);  // get
    g_rsx.context_ptr_addr = ctx_pp;
    vm::write32(ctx_pp, c);
    rsx_start();
    RET(CELL_OK);
}

HLE_FUNC(cellGcmSys, cellGcmSetDefaultCommandBuffer) {
    if (g_rsx.context_ptr_addr) vm::write32(g_rsx.context_ptr_addr, g_rsx.context_addr);
}
HLE_FUNC(cellGcmSys, _cellGcmFunc15) { RET(CELL_OK); }

HLE_FUNC(cellGcmSys, cellGcmGetControlRegister) { ctx.r[3] = g_rsx.control_addr; }
HLE_FUNC(cellGcmSys, cellGcmGetLabelAddress) { ctx.r[3] = g_rsx.label_addr + 0x10 * (ARG32(0) & 0xFF); }

// CellGcmConfig: localAddress, ioAddress, localSize, ioSize, memoryFrequency, coreFrequency
HLE_FUNC(cellGcmSys, cellGcmGetConfiguration) {
    uint32_t p = ARG32(0);
    vm::write32(p + 0, g_rsx.local_addr);
    vm::write32(p + 4, g_rsx.io_addr);
    vm::write32(p + 8, g_rsx.local_size);
    vm::write32(p + 12, g_rsx.io_size);
    vm::write32(p + 16, 650000000);
    vm::write32(p + 20, 500000000);
}

HLE_FUNC(cellGcmSys, cellGcmAddressToOffset) {
    uint32_t a = ARG32(0), out = ARG32(1), off;
    if (a >= g_rsx.local_addr && a < g_rsx.local_addr + g_rsx.local_size) off = a - g_rsx.local_addr;
    else if (a >= g_rsx.io_addr && a < g_rsx.io_addr + g_rsx.io_size) off = a - g_rsx.io_addr;
    else {
        LOG_W("gcm", "cellGcmAddressToOffset(0x%08X): not mapped", a);
        return (void)RET(CELL_GCM_ERROR_FAILURE);
    }
    vm::write32(out, off);
    RET(CELL_OK);
}

HLE_FUNC(cellGcmSys, cellGcmGetTiledPitchSize) {
    static const uint32_t pitches[] = {0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0xA00, 0xC00, 0xD00, 0xE00,
                                       0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
                                       0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000, 0xC000, 0xD000, 0xE000,
                                       0x10000};
    uint32_t size = ARG32(0);
    for (uint32_t p : pitches)
        if (p >= size) return (void)(ctx.r[3] = p);
    ctx.r[3] = 0;
}

HLE_FUNC(cellGcmSys, cellGcmSetFlipMode) { g_rsx.flip_mode = ARG32(0); }
HLE_FUNC(cellGcmSys, cellGcmSetDisplayBuffer) {
    uint32_t id = ARG32(0);
    if (id >= 8) return (void)RET(CELL_GCM_ERROR_INVALID_VALUE);
    g_rsx.displays[id] = {ARG32(1), ARG32(2), ARG32(3), ARG32(4)};
    LOG_I("gcm", "display buffer %u: offset 0x%08X pitch %u %ux%u", id, ARG32(1), ARG32(2), ARG32(3), ARG32(4));
    RET(CELL_OK);
}

// Write a flip command into the given context's command buffer.
static void write_flip(PPCContext& ctx, uint32_t c, uint32_t id) {
    uint32_t cur = vm::read32(c + 8), end = vm::read32(c + 4);
    if (cur + 8 > end) {
        uint64_t saved3 = ctx.r[3], saved4 = ctx.r[4];
        ctx.r[3] = c;
        ctx.r[4] = 2;
        call_guest_opd(ctx, vm::read32(c + 12));
        ctx.r[3] = saved3;
        ctx.r[4] = saved4;
        cur = vm::read32(c + 8);
    }
    vm::write32(cur, (1u << 18) | 0xFEAC);
    vm::write32(cur + 4, id);
    vm::write32(c + 8, cur + 8);
}
HLE_FUNC(cellGcmSys, _cellGcmSetFlipCommand) {
    write_flip(ctx, ARG32(0), ARG32(1));
    RET(CELL_OK);
}
// (context, id, label_index, label_value)
HLE_FUNC(cellGcmSys, _cellGcmSetFlipCommandWithWaitLabel) {
    write_flip(ctx, ARG32(0), ARG32(1));
    vm::write32(g_rsx.label_addr + 0x10 * (ARG32(2) & 0xFF), ARG32(3));
    RET(CELL_OK);
}
HLE_FUNC(cellGcmSys, cellGcmResetFlipStatus) {}
HLE_FUNC(cellGcmSys, cellGcmGetVBlankCount) { ctx.r[3] = system_time_us() * 60 / 1000000; }
HLE_FUNC(cellGcmSys, cellGcmSetUserHandler) { g_rsx.user_handler = ARG32(0); }
HLE_FUNC(cellGcmSys, cellGcmSetDebugOutputLevel) {}
HLE_FUNC(cellGcmSys, cellGcmSetTileInfo) { RET(CELL_OK); }
HLE_FUNC(cellGcmSys, cellGcmBindTile) { RET(CELL_OK); }
HLE_FUNC(cellGcmSys, cellGcmUnbindTile) { RET(CELL_OK); }
HLE_FUNC(cellGcmSys, cellGcmBindZcull) { RET(CELL_OK); }
HLE_FUNC(cellGcmSys, cellGcmUnbindZcull) { RET(CELL_OK); }
