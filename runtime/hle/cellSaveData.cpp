// cellSaveData (auto / list / fixed load+save), sysutil callback queue,
// cellMsgDialog and sceNpTrophy. Struct layouts follow RPCS3's cellSaveData.h.
#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

#include "../kernel/kernel.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------- sysutil callback queue
// System dialogs answer through callbacks that the game receives when it calls
// cellSysutilCheckCallback on its own thread.
namespace {
struct Pending {
    std::chrono::steady_clock::time_point due;
    std::function<void(PPCContext&)> fn;
};
std::mutex g_q_mutex;
std::deque<Pending> g_queue;

void queue_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, int delay_ms) {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_queue.push_back({std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms),
                       [opd, a0, a1](PPCContext& ctx) {
                           PPCContext saved = ctx;
                           ctx.r[3] = a0;
                           ctx.r[4] = a1;
                           call_guest_opd(ctx, opd);
                           ctx = saved;
                       }});
}

// Run a guest callback synchronously with up to 5 arguments; returns r3.
uint64_t guest_call(PPCContext& ctx, uint32_t opd, std::initializer_list<uint64_t> args) {
    PPCContext saved = ctx;
    int i = 0;
    for (uint64_t a : args) ctx.r[3 + i++] = a;
    call_guest_opd(ctx, opd);
    uint64_t r = ctx.r[3];
    ctx = saved;
    return r;
}

fs::path host_path(const std::string& guest) { return fs::path((const char8_t*)vfs_host_path(guest).c_str()); }
}  // namespace

HLE_FUNC(cellSysutil, cellSysutilCheckCallback) {
    std::vector<std::function<void(PPCContext&)>> run;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        auto now = std::chrono::steady_clock::now();
        while (!g_queue.empty() && g_queue.front().due <= now) {
            run.push_back(g_queue.front().fn);
            g_queue.pop_front();
        }
    }
    for (auto& f : run) f(ctx);
    RET(CELL_OK);
}

// ---------------------------------------------------------------- message dialog
// cellMsgDialogOpen2(type, msgString, callback, userData, extParam)
// type bits 4-6: button type (0 none, 1 yes/no, 2 ok). Dialogs with buttons are
// answered "yes/ok" after one second; the callback is (s32 buttonType, void* userData).
namespace {
uint32_t g_dlg_cb = 0, g_dlg_ud = 0;
}
HLE_FUNC(cellSysutil, cellMsgDialogOpen2) {
    uint32_t type = ARG32(0), cb = ARG32(2), ud = ARG32(3);
    std::string msg = vm::read_cstr(ARG32(1), 512);
    uint32_t buttons = (type >> 4) & 7;
    g_dlg_cb = cb;
    g_dlg_ud = ud;
    LOG_I("dialog", "system message (type 0x%X): \"%s\"%s", type, msg.c_str(), buttons ? " - answering YES/OK" : "");
    if (buttons && cb) queue_guest_call(cb, 1 /* CELL_MSGDIALOG_BUTTON_YES / OK */, ud, 1000);
    RET(CELL_OK);
}
HLE_FUNC(cellSysutil, cellMsgDialogOpenErrorCode) {
    LOG_I("dialog", "system error dialog 0x%08X", ARG32(0));
    if (ARG32(1)) queue_guest_call(ARG32(1), 1, ARG32(2), 1000);
    RET(CELL_OK);
}
HLE_FUNC(cellSysutil, cellMsgDialogClose) {
    if (g_dlg_cb) queue_guest_call(g_dlg_cb, (uint64_t)(int64_t)-1 /* CELL_MSGDIALOG_BUTTON_NONE */, g_dlg_ud, 0);
    g_dlg_cb = 0;
    RET(CELL_OK);
}
HLE_FUNC(cellSysutil, cellMsgDialogAbort) {
    g_dlg_cb = 0;
    RET(CELL_OK);
}
HLE_FUNC(cellSysutil, cellMsgDialogProgressBarInc) { RET(CELL_OK); }
HLE_FUNC(cellSysutil, cellMsgDialogProgressBarSetMsg) { RET(CELL_OK); }

// ---------------------------------------------------------------- trophies (accepted, logged, not stored)
HLE_FUNC(sceNpTrophy, sceNpTrophyInit) { RET(CELL_OK); }
HLE_FUNC(sceNpTrophy, sceNpTrophyTerm) { RET(CELL_OK); }
HLE_FUNC(sceNpTrophy, sceNpTrophyCreateContext) {
    vm::write32(ARG32(0), 1);
    RET(CELL_OK);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyDestroyContext) { RET(CELL_OK); }
HLE_FUNC(sceNpTrophy, sceNpTrophyCreateHandle) {
    vm::write32(ARG32(0), 1);
    RET(CELL_OK);
}
HLE_FUNC(sceNpTrophy, sceNpTrophyDestroyHandle) { RET(CELL_OK); }
// (context, handle, reqspace* u64, options)
HLE_FUNC(sceNpTrophy, sceNpTrophyGetRequiredDiskSpace) {
    vm::write64(ARG32(2), 0);
    RET(CELL_OK);
}
// (context, handle, statusCb(context, status, completed, total, arg), arg, options)
HLE_FUNC(sceNpTrophy, sceNpTrophyRegisterContext) {
    uint32_t context = ARG32(0), cb = ARG32(2), arg = ARG32(3);
    enum { SETUP = 6, PROGRESS = 7, FINALIZE = 8, COMPLETE = 9 };
    if (cb)
        for (int st : {SETUP, PROGRESS, FINALIZE, COMPLETE}) {
            int64_t r = (int32_t)guest_call(ctx, cb, {context, (uint64_t)st, st == SETUP ? 0u : 1u, 1, arg});
            if (r < 0) {
                LOG_W("trophy", "registration aborted by the game (%lld)", (long long)r);
                return (void)RET(0x80022910);  // SCE_NP_TROPHY_ERROR_ABORT
            }
        }
    LOG_I("trophy", "trophy context registered");
    RET(CELL_OK);
}
// (context, handle, trophyId, platinumId*)
HLE_FUNC(sceNpTrophy, sceNpTrophyUnlockTrophy) {
    LOG_I("trophy", "trophy %d unlocked", (int)ARG32(2));
    if (ARG32(3)) vm::write32(ARG32(3), 0xFFFFFFFF);  // no platinum
    RET(CELL_OK);
}

// ---------------------------------------------------------------- save data
namespace {
enum : uint32_t {
    CELL_SAVEDATA_RET_OK = 0, CELL_SAVEDATA_RET_CANCEL = 1,
    CELL_SAVEDATA_ERROR_CBRESULT = 0x8002B401, CELL_SAVEDATA_ERROR_ACCESS_ERROR = 0x8002B402,
    CELL_SAVEDATA_ERROR_NODATA = 0x8002B410,
};
const char* kSaveRoot = "/dev_hdd0/home/00000001/savedata/";
constexpr uint32_t kParamSize = 1452;  // CellSaveDataSystemFileParam

// scratch block for the structures handed to the callbacks
struct Scratch {
    uint32_t base, result, get, set, file_get, file_set, list_get, list_set, dir_list;
    Scratch() {
        base = vm::alloc(vm::Area::Kernel, 0x20000, 0x100);
        memset(vm::ptr(base), 0, 0x20000);
        result = base;             // CellSaveDataCBResult (20)
        get = base + 0x100;        // CellSaveDataStatGet (1604)
        set = base + 0x800;        // CellSaveDataStatSet
        file_get = base + 0x900;   // CellSaveDataFileGet
        file_set = base + 0xA00;   // CellSaveDataFileSet
        list_get = base + 0xB00;   // CellSaveDataListGet
        list_set = base + 0xC00;   // CellSaveDataListSet / FixedSet
        dir_list = base + 0x1000;  // CellSaveDataDirList[] (48 each)
    }
    ~Scratch() { vm::free(vm::Area::Kernel, base); }
};

std::vector<std::string> list_saves(const std::string& prefix) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path root = host_path(kSaveRoot);
    fs::create_directories(root, ec);
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        auto u = e.path().filename().u8string();
        std::string n(u.begin(), u.end());
        if (n.rfind(prefix, 0) == 0) out.push_back(n);
    }
    return out;
}

// funcStat + funcFile loop on one save directory
uint32_t run_save(PPCContext& ctx, Scratch& s, const std::string& dir, uint32_t func_stat, uint32_t func_file,
                  uint32_t userdata, bool saving) {
    std::error_code ec;
    fs::path host = host_path(kSaveRoot + dir);
    bool is_new = !fs::exists(host, ec);
    // ---- stat
    memset(vm::ptr(s.get), 0, 1604);
    vm::write32(s.get + 0, 40 * 1024 * 1024);
    vm::write32(s.get + 4, is_new ? 1 : 0);
    vm::write_cstr(s.get + 8 + 24, dir);
    if (!is_new) {
        FILE* f = fopen((host / "PARAM.BIN").string().c_str(), "rb");
        if (f) {
            fread(vm::ptr(s.get + 64), 1, kParamSize, f);
            fclose(f);
        }
        // file list
        uint32_t list = s.base + 0x8000, n = 0;
        for (auto& e : fs::directory_iterator(host, ec)) {
            auto u = e.path().filename().u8string();
            std::string name(u.begin(), u.end());
            if (name == "PARAM.BIN" || n >= 64) continue;
            uint32_t fe = list + 56 * n++;
            memset(vm::ptr(fe), 0, 56);
            vm::write32(fe, 0);
            vm::write64(fe + 8, (uint64_t)fs::file_size(e.path(), ec));
            vm::write_cstr(fe + 40, name.substr(0, 12));
        }
        vm::write32(s.get + 1528, n);
        vm::write32(s.get + 1532, n);
        vm::write32(s.get + 1536, list);
        vm::write32(s.get + 1520, 1024);
    }
    memset(vm::ptr(s.result), 0, 20);
    vm::write32(s.result + 16, userdata);
    memset(vm::ptr(s.set), 0, 16);
    guest_call(ctx, func_stat, {s.result, s.get, s.set});
    int32_t r = (int32_t)vm::read32(s.result);
    LOG_I("savedata", "  %s '%s' (%s): stat callback result %d", saving ? "save" : "load", dir.c_str(),
          is_new ? "new" : "existing", r);
    if (r < 0) return CELL_SAVEDATA_ERROR_CBRESULT;
    if (r == 1 || r == 2) {  // OK_LAST: nothing more to do
        if (is_new && !saving) return CELL_SAVEDATA_ERROR_NODATA;
    }
    uint32_t set_param = vm::read32(s.set);
    if (saving || set_param) fs::create_directories(host, ec);
    if (set_param) {
        FILE* f = fopen((host / "PARAM.BIN").string().c_str(), "wb");
        if (f) {
            fwrite(vm::ptr(set_param), 1, kParamSize, f);
            fclose(f);
        }
    }
    if (r == 1 || r == 2) return CELL_SAVEDATA_RET_OK;
    // ---- file operations
    for (int guard = 0; guard < 1000; ++guard) {
        memset(vm::ptr(s.result), 0, 20);
        vm::write32(s.result + 16, userdata);
        memset(vm::ptr(s.file_set), 0, 48);
        guest_call(ctx, func_file, {s.result, s.file_get, s.file_set});
        r = (int32_t)vm::read32(s.result);
        if (r < 0) return CELL_SAVEDATA_ERROR_CBRESULT;
        if (r == 1 || r == 2) return CELL_SAVEDATA_RET_OK;  // OK_LAST(_NOCONFIRM)
        uint32_t op = vm::read32(s.file_set), type = vm::read32(s.file_set + 8);
        std::string name = vm::read_cstr(vm::read32(s.file_set + 28), 64);
        static const char* sys_names[] = {"", "", "ICON0.PNG", "ICON1.PAM", "PIC1.PNG", "SND0.AT3"};
        if (type >= 2 && type <= 5) name = sys_names[type];
        uint32_t off = vm::read32(s.file_set + 32), size = vm::read32(s.file_set + 36);
        uint32_t buf = vm::read32(s.file_set + 44), buf_size = vm::read32(s.file_set + 40);
        fs::path fp = host / name;
        uint32_t done = 0;
        if (op == 0) {  // READ
            if (FILE* f = fopen(fp.string().c_str(), "rb")) {
                fseek(f, off, SEEK_SET);
                done = (uint32_t)fread(vm::ptr(buf), 1, std::min(size, buf_size), f);
                fclose(f);
            }
        } else if (op == 1 || op == 3) {  // WRITE / WRITE_NOTRUNC
            fs::create_directories(host, ec);
            FILE* f = fopen(fp.string().c_str(), op == 1 ? "wb" : (fs::exists(fp, ec) ? "r+b" : "wb"));
            if (f) {
                fseek(f, off, SEEK_SET);
                done = (uint32_t)fwrite(vm::ptr(buf), 1, size, f);
                fclose(f);
            }
        } else if (op == 2) {  // DELETE
            fs::remove(fp, ec);
        }
        vm::write32(s.file_get, done);
        LOG_I("savedata", "    file op %u '%s' offset %u size %u -> %u bytes", op, name.c_str(), off, size, done);
    }
    return CELL_SAVEDATA_ERROR_CBRESULT;
}

// list-style: funcList picks an entry (or new data), then run_save
uint32_t list_op(PPCContext& ctx, bool saving, uint32_t set_list, uint32_t func_list, uint32_t func_stat,
                 uint32_t func_file, uint32_t userdata, bool fixed) {
    Scratch s;
    std::string prefix = vm::read_cstr(vm::read32(set_list + 8), 64);
    auto saves = list_saves(prefix);
    memset(vm::ptr(s.list_get), 0, 76);
    vm::write32(s.list_get + 0, (uint32_t)saves.size());
    vm::write32(s.list_get + 4, (uint32_t)saves.size());
    vm::write32(s.list_get + 8, s.dir_list);
    for (size_t i = 0; i < saves.size() && i < 64; ++i) {
        uint32_t e = s.dir_list + 48 * (uint32_t)i;
        memset(vm::ptr(e), 0, 48);
        vm::write_cstr(e, saves[i]);
        if (FILE* f = fopen((host_path(kSaveRoot + saves[i]) / "PARAM.BIN").string().c_str(), "rb")) {
            uint8_t param[kParamSize];
            fread(param, 1, kParamSize, f);
            fclose(f);
            memcpy(vm::ptr(e + 32), param + 1288, 8);  // listParam
        }
    }
    memset(vm::ptr(s.result), 0, 20);
    vm::write32(s.result + 16, userdata);
    memset(vm::ptr(s.list_set), 0, 24);
    guest_call(ctx, func_list, {s.result, s.list_get, s.list_set});
    int32_t r = (int32_t)vm::read32(s.result);
    LOG_I("savedata", "%s %s (prefix '%s', %zu existing): list callback result %d", fixed ? "fixed" : "list",
          saving ? "save" : "load", prefix.c_str(), saves.size(), r);
    if (r < 0) return CELL_SAVEDATA_ERROR_CBRESULT;
    if (r == 1 || r == 2) return CELL_SAVEDATA_RET_OK;
    std::string dir;
    if (fixed) {
        dir = vm::read_cstr(vm::read32(s.list_set + 0), 64);  // CellSaveDataFixedSet.dirName
    } else {
        // the "user" picks the focused entry / first fixed entry, or the new-data slot when saving
        uint32_t focus = vm::read32(s.list_set + 4);
        uint32_t fixed_num = vm::read32(s.list_set + 8), fixed_list = vm::read32(s.list_set + 12);
        uint32_t new_data = vm::read32(s.list_set + 16);
        if (focus) dir = vm::read_cstr(focus, 64);
        if (dir.empty() && fixed_num && fixed_list) dir = vm::read_cstr(fixed_list, 32);
        if ((dir.empty() || saving) && new_data && vm::read32(new_data + 4)) {
            std::string nd = vm::read_cstr(vm::read32(new_data + 4), 64);
            if (saving && !nd.empty() && (dir.empty() || std::find(saves.begin(), saves.end(), dir) == saves.end())) dir = nd;
        }
    }
    if (dir.empty()) {
        LOG_I("savedata", "  nothing selected - cancel");
        return CELL_SAVEDATA_RET_CANCEL;
    }
    return run_save(ctx, s, dir, func_stat, func_file, userdata, saving);
}
}  // namespace

// (version, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata)
HLE_FUNC(cellSysutil, cellSaveDataAutoSave2) {
    Scratch s;
    std::string dir = vm::read_cstr(ARG32(1), 64);
    LOG_I("savedata", "auto save '%s'", dir.c_str());
    RET(run_save(ctx, s, dir, ARG32(4), ARG32(5), ARG32(7), true));
}
HLE_FUNC(cellSysutil, cellSaveDataAutoLoad2) {
    Scratch s;
    std::string dir = vm::read_cstr(ARG32(1), 64);
    LOG_I("savedata", "auto load '%s'", dir.c_str());
    RET(run_save(ctx, s, dir, ARG32(4), ARG32(5), ARG32(7), false));
}
// (version, setList, setBuf, funcList/funcFixed, funcStat, funcFile, container, userdata)
HLE_FUNC(cellSysutil, cellSaveDataListLoad2) { RET(list_op(ctx, false, ARG32(1), ARG32(3), ARG32(4), ARG32(5), ARG32(7), false)); }
HLE_FUNC(cellSysutil, cellSaveDataListSave2) { RET(list_op(ctx, true, ARG32(1), ARG32(3), ARG32(4), ARG32(5), ARG32(7), false)); }
HLE_FUNC(cellSysutil, cellSaveDataFixedLoad2) { RET(list_op(ctx, false, ARG32(1), ARG32(3), ARG32(4), ARG32(5), ARG32(7), true)); }
HLE_FUNC(cellSysutil, cellSaveDataFixedSave2) { RET(list_op(ctx, true, ARG32(1), ARG32(3), ARG32(4), ARG32(5), ARG32(7), true)); }
HLE_FUNC(cellSysutil, cellSaveDataDelete2) { RET(CELL_SAVEDATA_RET_CANCEL); }
