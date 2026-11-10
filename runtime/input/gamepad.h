// Host game controllers -> PS3 pad state.
//   DualShock 4 (USB / Bluetooth / wireless adapter) and DualSense (USB / Bluetooth) natively via HID,
//   anything XInput-compatible (Xbox pads, DS4Windows, Steam Input) as a fallback.
#pragma once

#include <cstddef>
#include <cstdint>

struct PadState {
    bool connected = false;
    uint16_t d1 = 0, d2 = 0;                        // CELL_PAD digital1 / digital2 bits
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;  // 0 = left/up, 255 = right/down
    uint8_t l2 = 0, r2 = 0;                          // trigger pressure
    const char* name = "";
};

// PS3 digital bits
enum : uint16_t {
    PAD1_LEFT = 0x80, PAD1_DOWN = 0x40, PAD1_RIGHT = 0x20, PAD1_UP = 0x10,
    PAD1_START = 0x08, PAD1_R3 = 0x04, PAD1_L3 = 0x02, PAD1_SELECT = 0x01,
    PAD2_SQUARE = 0x80, PAD2_CROSS = 0x40, PAD2_CIRCLE = 0x20, PAD2_TRIANGLE = 0x10,
    PAD2_R1 = 0x08, PAD2_L1 = 0x04, PAD2_R2 = 0x02, PAD2_L2 = 0x01,
};

void gamepad_start();        // starts the device thread (idempotent)
PadState gamepad_state();    // current state of the active controller
void gamepad_rumble(uint8_t small_motor, uint8_t big_motor);

// Report parsers (exposed for tests). `usb` selects the USB layout where it differs from Bluetooth.
bool parse_ds4_report(const uint8_t* buf, size_t len, PadState& out);
bool parse_dualsense_report(const uint8_t* buf, size_t len, bool usb, PadState& out);
