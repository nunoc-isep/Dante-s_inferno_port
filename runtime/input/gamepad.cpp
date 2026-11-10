// Native DualShock 4 / DualSense support over HID + XInput fallback.
//
// Input report layouts (report id at byte 0):
//   DS4 0x01 (USB, and the Bluetooth "simple" report), DS4 0x11 (Bluetooth full, data 2 bytes later):
//     LX LY RX RY | hat+face | L1 R1 L2 R2 share options L3 R3 | PS touch | L2 R2
//   DualSense 0x01 (USB, 64 bytes) and 0x31 (Bluetooth full, data 1 byte later):
//     LX LY RX RY L2 R2 counter | hat+face | L1 R1 L2 R2 create options L3 R3 | PS touch mute
//   DualSense Bluetooth "simple" 0x01 report uses the DS4 layout.
#include "gamepad.h"

#include <windows.h>
// clang-format off
#include <setupapi.h>
extern "C" {  // MinGW's HID headers lack C linkage declarations
#include <hidsdi.h>
#include <hidpi.h>
}
// clang-format on
#include <xinput.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../kernel/kernel.h"

namespace {

std::mutex g_mutex;
PadState g_state;
std::atomic<bool> g_started{false};

// rumble request (applied by the device thread)
std::atomic<uint32_t> g_rumble{0};
std::atomic<bool> g_rumble_dirty{false};

void apply_hat(uint8_t hat, PadState& s) {
    static const uint16_t dirs[8] = {PAD1_UP, PAD1_UP | PAD1_RIGHT, PAD1_RIGHT, PAD1_DOWN | PAD1_RIGHT,
                                     PAD1_DOWN, PAD1_DOWN | PAD1_LEFT, PAD1_LEFT, PAD1_UP | PAD1_LEFT};
    if (hat < 8) s.d1 |= dirs[hat];
}

// shared decoding of the face/shoulder bytes (same bit order on DS4 and DualSense)
void decode_buttons(uint8_t face, uint8_t shoulder, PadState& s) {
    apply_hat(face & 0x0F, s);
    if (face & 0x10) s.d2 |= PAD2_SQUARE;
    if (face & 0x20) s.d2 |= PAD2_CROSS;
    if (face & 0x40) s.d2 |= PAD2_CIRCLE;
    if (face & 0x80) s.d2 |= PAD2_TRIANGLE;
    if (shoulder & 0x01) s.d2 |= PAD2_L1;
    if (shoulder & 0x02) s.d2 |= PAD2_R1;
    if (shoulder & 0x04) s.d2 |= PAD2_L2;
    if (shoulder & 0x08) s.d2 |= PAD2_R2;
    if (shoulder & 0x10) s.d1 |= PAD1_SELECT;  // Share / Create
    if (shoulder & 0x20) s.d1 |= PAD1_START;   // Options
    if (shoulder & 0x40) s.d1 |= PAD1_L3;
    if (shoulder & 0x80) s.d1 |= PAD1_R3;
}

}  // namespace

bool parse_ds4_report(const uint8_t* b, size_t len, PadState& s) {
    size_t base;
    if (len >= 10 && b[0] == 0x01) base = 1;
    else if (len >= 12 && b[0] == 0x11) base = 3;
    else return false;
    s = PadState{};
    s.connected = true;
    s.lx = b[base + 0];
    s.ly = b[base + 1];
    s.rx = b[base + 2];
    s.ry = b[base + 3];
    decode_buttons(b[base + 4], b[base + 5], s);
    s.l2 = b[base + 7];
    s.r2 = b[base + 8];
    return true;
}

bool parse_dualsense_report(const uint8_t* b, size_t len, bool usb, PadState& s) {
    if (b[0] == 0x01 && !usb) return parse_ds4_report(b, len, s);  // Bluetooth simple report
    size_t base;
    if (b[0] == 0x01 && len >= 11) base = 1;
    else if (b[0] == 0x31 && len >= 12) base = 2;
    else return false;
    s = PadState{};
    s.connected = true;
    s.lx = b[base + 0];
    s.ly = b[base + 1];
    s.rx = b[base + 2];
    s.ry = b[base + 3];
    s.l2 = b[base + 4];
    s.r2 = b[base + 5];
    decode_buttons(b[base + 7], b[base + 8], s);
    return true;
}

namespace {

enum class Kind { DS4, DualSense };

struct HidPad {
    HANDLE h = INVALID_HANDLE_VALUE;
    Kind kind;
    bool usb;
    std::vector<uint8_t> buf;
    size_t out_len = 0;
    std::string name;
};

bool sony_kind(USHORT vid, USHORT pid, Kind& k) {
    if (vid != 0x054C) return false;
    switch (pid) {
        case 0x05C4: case 0x09CC: case 0x0BA0: k = Kind::DS4; return true;
        case 0x0CE6: case 0x0DF2: k = Kind::DualSense; return true;
        default: return false;
    }
}

bool open_sony_pad(HidPad& pad) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> mem(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)mem.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{sizeof(attr)};
        Kind k;
        if (HidD_GetAttributes(h, &attr) && sony_kind(attr.VendorID, attr.ProductID, k)) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            HIDP_CAPS caps{};
            if (HidD_GetPreparsedData(h, &pp)) {
                HidP_GetCaps(pp, &caps);
                HidD_FreePreparsedData(pp);
            }
            if (caps.InputReportByteLength) {
                pad.h = h;
                pad.kind = k;
                pad.usb = caps.InputReportByteLength == 64;
                pad.buf.assign(caps.InputReportByteLength, 0);
                pad.out_len = caps.OutputReportByteLength;
                pad.name = std::string(k == Kind::DS4 ? "DualShock 4" : "DualSense") + (pad.usb ? " (USB)" : " (Bluetooth)");
                found = true;
                continue;
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// USB output report: rumble + lightbar. Bluetooth output needs a CRC'd report and is not sent yet.
void send_output(HidPad& pad, uint8_t small, uint8_t big) {
    if (!pad.usb || !pad.out_len) return;
    std::vector<uint8_t> r(pad.out_len, 0);
    if (pad.kind == Kind::DS4) {
        r[0] = 0x05;
        r[1] = 0x07;  // rumble + lightbar
        r[4] = small;
        r[5] = big;
        r[6] = 0x60;  // dark red lightbar
        r[7] = 0x00;
        r[8] = 0x00;
    } else {
        r[0] = 0x02;
        r[1] = 0x03;  // compatible vibration + haptics select
        r[2] = 0x04;  // lightbar control
        r[3] = small;
        r[4] = big;
        if (r.size() > 47) {
            r[45] = 0x60;  // lightbar red
            r[46] = 0x00;
            r[47] = 0x00;
        }
    }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    if (!WriteFile(pad.h, r.data(), (DWORD)r.size(), &written, &ov) && GetLastError() == ERROR_IO_PENDING)
        GetOverlappedResult(pad.h, &ov, &written, TRUE);
    CloseHandle(ov.hEvent);
}

// XInput, loaded dynamically (xinput1_4 on Windows 8+, 9_1_0 otherwise)
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* XInputSetStateFn)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_xget;
XInputSetStateFn g_xset;

uint8_t axis(SHORT v, bool invert) {
    int x = invert ? -(int)v : (int)v;
    x = (x + 32768) >> 8;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

bool poll_xinput(PadState& s, int& slot) {
    if (!g_xget) return false;
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st;
        if (g_xget(i, &st) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD& g = st.Gamepad;
        s = PadState{};
        s.connected = true;
        s.name = "XInput controller";
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) s.d1 |= PAD1_UP;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) s.d1 |= PAD1_DOWN;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) s.d1 |= PAD1_LEFT;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) s.d1 |= PAD1_RIGHT;
        if (g.wButtons & XINPUT_GAMEPAD_START) s.d1 |= PAD1_START;
        if (g.wButtons & XINPUT_GAMEPAD_BACK) s.d1 |= PAD1_SELECT;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) s.d1 |= PAD1_L3;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) s.d1 |= PAD1_R3;
        if (g.wButtons & XINPUT_GAMEPAD_A) s.d2 |= PAD2_CROSS;
        if (g.wButtons & XINPUT_GAMEPAD_B) s.d2 |= PAD2_CIRCLE;
        if (g.wButtons & XINPUT_GAMEPAD_X) s.d2 |= PAD2_SQUARE;
        if (g.wButtons & XINPUT_GAMEPAD_Y) s.d2 |= PAD2_TRIANGLE;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) s.d2 |= PAD2_L1;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) s.d2 |= PAD2_R1;
        s.l2 = g.bLeftTrigger;
        s.r2 = g.bRightTrigger;
        if (s.l2 > 30) s.d2 |= PAD2_L2;
        if (s.r2 > 30) s.d2 |= PAD2_R2;
        s.lx = axis(g.sThumbLX, false);
        s.ly = axis(g.sThumbLY, true);  // XInput: up is positive
        s.rx = axis(g.sThumbRX, false);
        s.ry = axis(g.sThumbRY, true);
        slot = (int)i;
        return true;
    }
    return false;
}

void device_thread() {
    HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
    if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
    if (xi) {
        g_xget = (XInputGetStateFn)GetProcAddress(xi, "XInputGetState");
        g_xset = (XInputSetStateFn)GetProcAddress(xi, "XInputSetState");
    }
    HidPad pad;
    DWORD last_scan = 0;
    std::string announced;
    int xslot = -1;
    for (;;) {
        // ---- native Sony controller
        if (pad.h == INVALID_HANDLE_VALUE && GetTickCount() - last_scan > 2000) {
            last_scan = GetTickCount();
            if (open_sony_pad(pad)) {
                LOG_I("pad", "%s connected", pad.name.c_str());
                send_output(pad, 0, 0);
            }
        }
        if (pad.h != INVALID_HANDLE_VALUE) {
            if (g_rumble_dirty.exchange(false)) {
                uint32_t r = g_rumble;
                send_output(pad, (uint8_t)(r >> 8), (uint8_t)r);
            }
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD got = 0;
            bool ok = ReadFile(pad.h, pad.buf.data(), (DWORD)pad.buf.size(), &got, &ov) != 0;
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0) ok = GetOverlappedResult(pad.h, &ov, &got, FALSE) != 0;
                else {
                    CancelIo(pad.h);
                    GetOverlappedResult(pad.h, &ov, &got, TRUE);
                    got = 0;
                    ok = true;  // just no report within 100 ms
                }
            }
            CloseHandle(ov.hEvent);
            if (!ok) {
                LOG_I("pad", "%s disconnected", pad.name.c_str());
                CloseHandle(pad.h);
                pad = HidPad{};
                std::lock_guard<std::mutex> lk(g_mutex);
                g_state = PadState{};
                continue;
            }
            if (got) {
                PadState s;
                bool parsed = pad.kind == Kind::DS4 ? parse_ds4_report(pad.buf.data(), got, s)
                                                    : parse_dualsense_report(pad.buf.data(), got, pad.usb, s);
                if (parsed) {
                    s.name = pad.kind == Kind::DS4 ? "DualShock 4" : "DualSense";
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_state = s;
                }
            }
            continue;
        }
        // ---- XInput fallback
        PadState s;
        int slot = -1;
        if (poll_xinput(s, slot)) {
            if (announced != "xinput") {
                LOG_I("pad", "XInput controller connected (slot %d)", slot);
                announced = "xinput";
            }
            xslot = slot;
            if (g_rumble_dirty.exchange(false) && g_xset) {
                uint32_t r = g_rumble;
                XINPUT_VIBRATION v{(WORD)((r & 0xFF) * 257), (WORD)(((r >> 8) & 0xFF) * 257)};
                g_xset((DWORD)xslot, &v);
            }
        } else {
            if (announced == "xinput") LOG_I("pad", "XInput controller disconnected");
            announced.clear();
            s = PadState{};
        }
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_state = s;
        }
        Sleep(8);
    }
}

}  // namespace

void gamepad_start() {
    if (g_started.exchange(true)) return;
    std::thread(device_thread).detach();
}

PadState gamepad_state() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_state;
}

void gamepad_rumble(uint8_t small_motor, uint8_t big_motor) {
    uint32_t v = ((uint32_t)small_motor << 8) | big_motor;
    if (g_rumble.exchange(v) != v) g_rumble_dirty = true;
}
