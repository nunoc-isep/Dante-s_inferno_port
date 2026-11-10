// sys_io: one standard controller on port 0, driven by the keyboard while the
// game window (or the console) has focus. cellKb reports no keyboard.
//   arrows = D-pad, Enter = START, Backspace = SELECT, Z = CROSS, X = CIRCLE,
//   A = SQUARE, S = TRIANGLE, Q/W = L1/R1, 1/2 = L2/R2
#include <windows.h>

#include "../gpu/gpu.h"
#include "../kernel/kernel.h"

namespace {
bool g_pad_init = false;
enum : uint32_t { CELL_PAD_ERROR_UNINITIALIZED = 0x80121103, CELL_PAD_ERROR_INVALID_PARAMETER = 0x80121104,
                  CELL_PAD_ERROR_NO_DEVICE = 0x80121107 };

bool key(int vk) {
    HWND fg = GetForegroundWindow();
    return fg && (fg == GetConsoleWindow() || fg == g_gpu.hwnd) && (GetAsyncKeyState(vk) & 0x8000);
}
}  // namespace

HLE_FUNC(sys_io, cellPadInit) {
    g_pad_init = true;
    LOG_I("pad", "cellPadInit(max %u): controller 0 mapped to the keyboard (console window focus)", ARG32(0));
    RET(CELL_OK);
}
HLE_FUNC(sys_io, cellPadEnd) {
    g_pad_init = false;
    RET(CELL_OK);
}

// CellPadInfo2: u32 max_connect, now_connect, system_info, port_status[7], port_setting[7],
//               device_capability[7], device_type[7]
HLE_FUNC(sys_io, cellPadGetInfo2) {
    if (!g_pad_init) return (void)RET(CELL_PAD_ERROR_UNINITIALIZED);
    uint32_t p = ARG32(0);
    memset(vm::ptr(p), 0, 12 + 4 * 28);
    vm::write32(p + 0, 7);
    vm::write32(p + 4, 1);
    vm::write32(p + 12, 1);              // port 0: connected
    vm::write32(p + 12 + 28 * 2, 0x7);   // capability: PS3 conformity | press | sensor
    vm::write32(p + 12 + 28 * 3, 0);     // standard controller
    RET(CELL_OK);
}

// CellPadData: s32 len, u16 button[64]
HLE_FUNC(sys_io, cellPadGetData) {
    if (!g_pad_init) return (void)RET(CELL_PAD_ERROR_UNINITIALIZED);
    uint32_t port = ARG32(0), d = ARG32(1);
    memset(vm::ptr(d), 0, 4 + 2 * 64);
    if (port != 0) return (void)RET(CELL_PAD_ERROR_NO_DEVICE);
    uint16_t d1 = 0, d2 = 0;
    if (key(VK_LEFT)) d1 |= 0x80;
    if (key(VK_DOWN)) d1 |= 0x40;
    if (key(VK_RIGHT)) d1 |= 0x20;
    if (key(VK_UP)) d1 |= 0x10;
    if (key(VK_RETURN)) d1 |= 0x08;
    if (key(VK_BACK)) d1 |= 0x01;
    if (key('A')) d2 |= 0x80;
    if (key('Z')) d2 |= 0x40;
    if (key('X')) d2 |= 0x20;
    if (key('S')) d2 |= 0x10;
    if (key('W')) d2 |= 0x08;
    if (key('Q')) d2 |= 0x04;
    if (key('2')) d2 |= 0x02;
    if (key('1')) d2 |= 0x01;
    vm::write32(d, 24);
    uint32_t b = d + 4;
    vm::write16(b + 2 * 1, 0x0070);  // number of valid data blocks (upper nibble), as reported by real pads
    vm::write16(b + 2 * 2, d1);
    vm::write16(b + 2 * 3, d2);
    for (int i = 4; i < 8; ++i) vm::write16(b + 2 * i, 0x80);  // sticks centred
    // pressure values for the pressed digital buttons
    static const struct { int idx; uint16_t* reg; uint16_t bit; } press[] = {
        {8, &d1, 0x20}, {9, &d1, 0x80}, {10, &d1, 0x10}, {11, &d1, 0x40}, {12, &d2, 0x10}, {13, &d2, 0x20},
        {14, &d2, 0x40}, {15, &d2, 0x80}, {16, &d2, 0x04}, {17, &d2, 0x08}, {18, &d2, 0x01}, {19, &d2, 0x02}};
    for (auto& p : press) vm::write16(b + 2 * p.idx, (*p.reg & p.bit) ? 0xFF : 0);
    for (int i = 20; i < 23; ++i) vm::write16(b + 2 * i, 0x200);  // motion sensors at rest
    vm::write16(b + 2 * 23, 0x200);
    RET(CELL_OK);
}

HLE_FUNC(sys_io, cellPadSetPortSetting) { RET(CELL_OK); }
HLE_FUNC(sys_io, cellPadSetActDirect) { RET(CELL_OK); }

// keyboard library: initialised, but no keyboard connected
HLE_FUNC(sys_io, cellKbInit) { RET(CELL_OK); }
HLE_FUNC(sys_io, cellKbEnd) { RET(CELL_OK); }
// CellKbInfo: u32 max_connect, now_connect, info, u8 status[7]
HLE_FUNC(sys_io, cellKbGetInfo) {
    memset(vm::ptr(ARG32(0)), 0, 20);
    vm::write32(ARG32(0), 7);
    RET(CELL_OK);
}
HLE_FUNC(sys_io, cellKbRead) { RET(0x80121007 /* CELL_KB_ERROR_NO_DEVICE */); }
HLE_FUNC(sys_io, cellKbClearBuf) { RET(CELL_OK); }
HLE_FUNC(sys_io, cellKbSetCodeType) { RET(CELL_OK); }
HLE_FUNC(sys_io, cellKbSetReadMode) { RET(CELL_OK); }
HLE_FUNC(sys_io, cellKbSetLEDStatus) { RET(CELL_OK); }

// CellSysutilBgmPlaybackStatus (32 bytes): u8 playerState, u8 enableState, char contentsId[16],
// u8 currentFadeRatio, char reserved[13]. Writing more would clobber the caller's stack frame.
HLE_FUNC(cellSysutil, cellSysutilGetBgmPlaybackStatus) {
    memset(vm::ptr(ARG32(0)), 0, 0x20);  // not playing
    RET(CELL_OK);
}
