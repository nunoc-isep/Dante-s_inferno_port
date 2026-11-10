// Report-parser tests for DualShock 4 / DualSense (synthetic reports, hand-computed expectations).
#include <cstdio>
#include <cstring>
#include <vector>

#include "input/gamepad.h"

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

int main() {
    PadState s;
    // DS4 USB: sticks 10/20/30/40, hat 1 (up-right) + cross, L1 + options (Start) + R3, L2 200, R2 100
    {
        std::vector<uint8_t> r(64, 0);
        r[0] = 0x01; r[1] = 10; r[2] = 20; r[3] = 30; r[4] = 40;
        r[5] = 0x01 | 0x20;   // hat up-right, cross
        r[6] = 0x01 | 0x20 | 0x80;  // L1, options, R3
        r[8] = 200; r[9] = 100;
        bool ok = parse_ds4_report(r.data(), r.size(), s);
        check(ok && s.lx == 10 && s.ly == 20 && s.rx == 30 && s.ry == 40, "DS4 USB sticks");
        check(s.d1 == (PAD1_UP | PAD1_RIGHT | PAD1_START | PAD1_R3), "DS4 USB d-pad + Start + R3");
        check(s.d2 == (PAD2_CROSS | PAD2_L1), "DS4 USB cross + L1");
        check(s.l2 == 200 && s.r2 == 100, "DS4 USB triggers");
    }
    // DS4 Bluetooth full report 0x11: same data two bytes later; hat 8 = released, triangle + share (Select)
    {
        std::vector<uint8_t> r(78, 0);
        r[0] = 0x11; r[3] = 128; r[4] = 128; r[5] = 255; r[6] = 0;
        r[7] = 0x08 | 0x80;  // no direction, triangle
        r[8] = 0x10;         // share
        bool ok = parse_ds4_report(r.data(), r.size(), s);
        check(ok && s.lx == 128 && s.rx == 255 && s.ry == 0, "DS4 Bluetooth 0x11 sticks");
        check(s.d1 == PAD1_SELECT && s.d2 == PAD2_TRIANGLE, "DS4 Bluetooth 0x11 buttons, hat released");
    }
    // DualSense USB: triggers before the buttons; hat 6 (left), square + circle, R1 + L2 + create
    {
        std::vector<uint8_t> r(64, 0);
        r[0] = 0x01; r[1] = 1; r[2] = 2; r[3] = 3; r[4] = 4; r[5] = 77; r[6] = 88;
        r[8] = 0x06 | 0x10 | 0x40;  // left, square, circle
        r[9] = 0x02 | 0x04 | 0x10;  // R1, L2, create
        bool ok = parse_dualsense_report(r.data(), r.size(), true, s);
        check(ok && s.lx == 1 && s.ly == 2 && s.rx == 3 && s.ry == 4, "DualSense USB sticks");
        check(s.l2 == 77 && s.r2 == 88, "DualSense USB triggers");
        check(s.d1 == (PAD1_LEFT | PAD1_SELECT) && s.d2 == (PAD2_SQUARE | PAD2_CIRCLE | PAD2_R1 | PAD2_L2),
              "DualSense USB buttons");
    }
    // DualSense Bluetooth full 0x31: data one byte later; hat 4 (down) + L3
    {
        std::vector<uint8_t> r(78, 0);
        r[0] = 0x31; r[2] = 50; r[3] = 60; r[4] = 70; r[5] = 80; r[6] = 5; r[7] = 6;
        r[9] = 0x04;
        r[10] = 0x40;
        bool ok = parse_dualsense_report(r.data(), r.size(), false, s);
        check(ok && s.lx == 50 && s.ry == 80 && s.l2 == 5 && s.r2 == 6, "DualSense Bluetooth 0x31 axes");
        check(s.d1 == (PAD1_DOWN | PAD1_L3) && s.d2 == 0, "DualSense Bluetooth 0x31 buttons");
    }
    // DualSense Bluetooth simple 0x01 uses the DS4 layout
    {
        std::vector<uint8_t> r(10, 0);
        r[0] = 0x01; r[1] = 9; r[5] = 0x08 | 0x20; r[8] = 33;
        bool ok = parse_dualsense_report(r.data(), r.size(), false, s);
        check(ok && s.lx == 9 && s.d2 == PAD2_CROSS && s.l2 == 33, "DualSense Bluetooth simple report");
    }
    printf(fails ? "%d gamepad test(s) FAILED\n" : "all gamepad tests passed\n", fails);
    return fails != 0;
}
