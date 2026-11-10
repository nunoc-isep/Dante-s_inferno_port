// cellSysutil (video/audio out, system parameters, callbacks), cellSysmodule, misc.
#include "../kernel/kernel.h"
#include "../kernel/sync.h"

#include <filesystem>

// ---------------------------------------------------------------- video out (720p, 16:9, 59.94 Hz)
static const uint8_t kResolutionId = 2;  // CELL_VIDEO_OUT_RESOLUTION_720

// CellVideoOutState: u8 state, u8 colorSpace, u8 reserved[6],
//   CellVideoOutDisplayMode { u8 resolutionId, scanMode, conversion, aspect, reserved[2], u16 refreshRates }
HLE_FUNC(cellSysutil, cellVideoOutGetState) {
    uint32_t p = ARG32(2);
    memset(vm::ptr(p), 0, 16);
    vm::write8(p + 0, 2);  // CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED
    vm::write8(p + 1, 1);  // RGB
    vm::write8(p + 8, kResolutionId);
    vm::write8(p + 9, 0);   // progressive
    vm::write8(p + 10, 0);  // no conversion
    vm::write8(p + 11, 2);  // 16:9
    vm::write16(p + 14, 4);  // 59.94 Hz
    RET(CELL_OK);
}
// CellVideoOutResolution: u16 width, u16 height
HLE_FUNC(cellSysutil, cellVideoOutGetResolution) {
    static const uint16_t res[][2] = {{0, 0}, {1920, 1080}, {1280, 720}, {0, 0}, {720, 480}, {720, 576}};
    uint32_t id = ARG32(0);
    if (id >= 6 || !res[id][0]) return (void)RET(0x8002B222 /* unsupported */);
    vm::write16(ARG32(1), res[id][0]);
    vm::write16(ARG32(1) + 2, res[id][1]);
    RET(CELL_OK);
}
HLE_FUNC(cellSysutil, cellVideoOutGetResolutionAvailability) {
    uint32_t id = ARG32(1);
    ctx.r[3] = (id == 1 || id == 2 || id == 4 || id == 5) ? 1 : 0;
}
HLE_FUNC(cellSysutil, cellVideoOutConfigure) {
    LOG_I("sysutil", "cellVideoOutConfigure(resolution %u, format %u, aspect %u, pitch %u)", vm::read8(ARG32(1)),
          vm::read8(ARG32(1) + 1), vm::read8(ARG32(1) + 2), vm::read32(ARG32(1) + 12));
    RET(CELL_OK);
}
HLE_FUNC(cellSysutilAvconfExt, cellVideoOutSetGamma) { RET(CELL_OK); }

// ---------------------------------------------------------------- audio out (stereo LPCM)
HLE_FUNC(cellSysutil, cellAudioOutGetSoundAvailability) {
    uint32_t type = ARG32(1);  // 0 = LPCM
    ctx.r[3] = type == 0 ? 2 : 0;
}
HLE_FUNC(cellSysutil, cellAudioOutConfigure) { RET(CELL_OK); }
// CellAudioOutDeviceInfo (0x88 bytes): u8 portType, availableModeCount, state, reserved[3]; u16 latency;
//   CellAudioOutSoundMode availableModes[16] at +8 (each: u8 type, channel, fs, reserved; u32 layout)
HLE_FUNC(cellSysutil, cellAudioOutGetDeviceInfo) {
    uint32_t p = ARG32(2);
    memset(vm::ptr(p), 0, 0x88);
    vm::write8(p + 0, 0);   // HDMI
    vm::write8(p + 1, 1);   // one mode
    vm::write8(p + 2, 2);   // available
    vm::write16(p + 6, 1000);
    // mode 0: type LPCM, 2 channels, 48 kHz
    vm::write8(p + 8, 0);
    vm::write8(p + 9, 2);
    vm::write8(p + 10, 4);  // CELL_AUDIO_OUT_FS_48KHZ
    vm::write32(p + 12, 1);  // layout: 2ch
    RET(CELL_OK);
}

// ---------------------------------------------------------------- system parameters
HLE_FUNC(cellSysutil, cellSysutilGetSystemParamInt) {
    uint32_t id = ARG32(0), out = ARG32(1);
    int32_t v = 0;
    switch (id) {
        case 0x0111: v = 1; break;  // language: English (US)
        case 0x0112: v = 1; break;  // enter button: cross
        case 0x0114: v = 2; break;  // date format: MMDDYYYY
        case 0x0115: v = 0; break;  // time format: 12h
        case 0x0116: v = 0; break;  // time zone (minutes)
        case 0x0117: v = 0; break;  // summer time
        case 0x0121: v = 0; break;  // game parental level: off
        case 0x0123: v = 0; break;  // game parental level0 restrict
        case 0x0141: v = 0; break;  // current user has NP account
        case 0x0151: v = 0; break;  // camera PLFREQ
        case 0x0152: v = 0; break;  // pad rumble
        case 0x0153: v = 0; break;  // keyboard type
        case 0x0154: v = 0; break;  // Japanese keyboard entry method
        case 0x0155: v = 0; break;  // Chinese keyboard entry method
        case 0x0156: v = 0; break;  // pad auto-off
        default: LOG_W("sysutil", "unknown system param 0x%X", id); break;
    }
    vm::write32(out, (uint32_t)v);
    RET(CELL_OK);
}

// ---------------------------------------------------------------- callbacks (no system events yet)
static uint32_t g_sysutil_cb[4], g_sysutil_ud[4];
HLE_FUNC(cellSysutil, cellSysutilRegisterCallback) {
    uint32_t slot = ARG32(0);
    if (slot >= 4) return (void)RET(CELL_EINVAL);
    g_sysutil_cb[slot] = ARG32(1);
    g_sysutil_ud[slot] = ARG32(2);
    RET(CELL_OK);
}
HLE_FUNC(cellSysutil, cellSysutilUnregisterCallback) {
    uint32_t slot = ARG32(0);
    if (slot < 4) g_sysutil_cb[slot] = 0;
    RET(CELL_OK);
}
HLE_FUNC(cellSysutil, cellSysutilEnableBgmPlayback) { RET(CELL_OK); }
HLE_FUNC(cellSysutil, cellSysutilDisableBgmPlayback) { RET(CELL_OK); }

// ---------------------------------------------------------------- cache / game data
// cellSysCacheMount(CellSysCacheParam*): char cacheId[32], char getCachePath[1024], reserved
HLE_FUNC(cellSysutil, cellSysCacheMount) {
    uint32_t p = ARG32(0);
    std::string id = vm::read_cstr(p, 32);
    std::string guest = "/dev_hdd1/caches/" + (id.empty() ? std::string("default") : id);
    vm::write_cstr(p + 32, guest);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path((const char8_t*)vfs_host_path(guest).c_str()), ec);
    LOG_I("sysutil", "cellSysCacheMount('%s') -> %s", id.c_str(), guest.c_str());
    RET(1);  // CELL_SYSCACHE_RET_OK_RELAYED (new cache)
}
HLE_FUNC(cellSysutil, cellSysCacheClear) { RET(CELL_OK); }

// ---------------------------------------------------------------- cellSysmodule
HLE_FUNC(cellSysmodule, cellSysmoduleLoadModule) {
    LOG_I("sysmodule", "load module 0x%04X", ARG32(0));
    RET(CELL_OK);
}
HLE_FUNC(cellSysmodule, cellSysmoduleUnloadModule) { RET(CELL_OK); }
HLE_FUNC(cellSysmodule, cellSysmoduleIsLoaded) { RET(CELL_OK); }

// ---------------------------------------------------------------- game data
// cellGameDataCheckCreate2(version, dirName, errDialog, funcStat (OPD), container)
// Calls funcStat(cbResult, get, set) synchronously, as the system utility does.
// Layouts (RPCS3 cellGame.h): CellGameDataCBResult {s32 result, errNeedSizeKB; u32 invalidMsg, reserved}
// CellGameDataStatGet: s32 hddFreeSizeKB @0, u32 isNewData @4, char contentInfoPath[1055] @8,
//   char gameDataPath[1055] @1063, pad, s64 atime/mtime/ctime @2120, CellGameDataSystemFileParam @2144
//   (title[128], titleLang[20][128], titleId[10]+2, dataVersion[6]+2, u32 parentalLevel, attribute, ...),
//   s32 sizeKB @5116, sysSizeKB @5120
HLE_FUNC(cellSysutil, cellGameDataCheckCreate2) {
    std::string dir_name = vm::read_cstr(ARG32(1), 64);
    uint32_t func_stat = ARG32(3);
    std::string guest_dir = "/dev_hdd0/game/" + dir_name;
    std::error_code ec;
    std::filesystem::path host((const char8_t*)vfs_host_path(guest_dir).c_str());
    bool is_new = !std::filesystem::exists(host, ec);

    uint32_t mem = vm::alloc(vm::Area::Kernel, 0x4000, 0x100);
    memset(vm::ptr(mem), 0, 0x4000);
    uint32_t result = mem, get = mem + 0x100, set = mem + 0x1800;
    vm::write32(get + 0, 40 * 1024 * 1024);  // 40 GB free
    vm::write32(get + 4, is_new ? 1 : 0);    // CELL_GAMEDATA_ISNEWDATA_YES / NO
    vm::write_cstr(get + 8, guest_dir);
    vm::write_cstr(get + 1063, guest_dir + "/USRDIR");
    vm::write_cstr(get + 2144, "Dante's Inferno");
    vm::write_cstr(get + 2144 + 128 + 2560, "BLUS30405");
    vm::write_cstr(get + 2144 + 128 + 2560 + 12, "01.00");
    vm::write32(get + 5116, (uint32_t)-1);  // CELL_GAMEDATA_SIZEKB_NOTCALC
    LOG_I("gamedata", "cellGameDataCheckCreate2('%s'): %s data, calling the game's callback", dir_name.c_str(),
          is_new ? "new" : "existing");

    PPCContext saved = ctx;  // the callback runs on this thread's guest stack
    ctx.r[3] = result;
    ctx.r[4] = get;
    ctx.r[5] = set;
    call_guest_opd(ctx, func_stat);
    int32_t cb = (int32_t)vm::read32(result);
    PPCContext after = ctx;
    ctx = saved;
    (void)after;
    vm::free(vm::Area::Kernel, mem);
    LOG_I("gamedata", "  callback result %d", cb);
    if (cb == 0 || cb == 1) {  // OK / OK_CANCEL
        if (cb == 0) std::filesystem::create_directories(host / "USRDIR", ec);
        return (void)RET(CELL_OK);
    }
    RET(0x8002B60A);  // CELL_GAMEDATA_ERROR_CBRESULT
}
HLE_FUNC(cellSysutil, cellGameDataGetSizeKB) {
    if (ARG32(0)) vm::write32(ARG32(0), 0);
    RET(CELL_OK);
}
