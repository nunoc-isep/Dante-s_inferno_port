// Dante's Inferno PC port - boot.
//
// Usage: dante.exe [--game <dump root>] [--user <dir>] [--trace] [--dump-after <seconds>]
//   <dump root> is the folder containing PS3_GAME/. If omitted, a folder named
//   "Dante's Inferno" (or any folder containing PS3_GAME/) is searched for next
//   to and above the executable, then next to and above the current folder.
//   PS3_GAME/USRDIR/EBOOT.elf must be the RPCS3-decrypted executable.
#include <windows.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "kernel/kernel.h"

namespace fs = std::filesystem;

static std::string to_utf8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

static fs::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return fs::path(std::wstring(buf, n)).parent_path();
}

static bool is_dump(const fs::path& d) {
    std::error_code ec;
    return fs::exists(d / "PS3_GAME" / "USRDIR", ec);
}

// Look for the disc dump near the executable and the current folder.
static fs::path find_game_root(std::vector<fs::path>& searched) {
    std::vector<fs::path> bases = {exe_dir(), fs::current_path()};
    for (const auto& start : bases) {
        fs::path d = start;
        for (int up = 0; up < 4 && !d.empty(); ++up) {
            for (const fs::path& c : {d, d / "Dante's Inferno"}) {
                searched.push_back(c);
                if (is_dump(c)) return c;
            }
            if (d == d.parent_path()) break;
            d = d.parent_path();
        }
    }
    return {};
}

int main(int argc, char** argv) {
    std::string game_root, user_root;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game_root = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user_root = argv[++i];
        else if (!strcmp(argv[i], "--trace")) g_log_level = LogLevel::Trace;
        else if (!strcmp(argv[i], "--dump-after") && i + 1 < argc) {
            // diagnostics: print every guest thread's recent call trace after N seconds
            int secs = atoi(argv[++i]);
            std::thread([secs] {
                std::this_thread::sleep_for(std::chrono::seconds(secs));
                dump_all_threads();
            }).detach();
        }
    }
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (game_root.empty()) {
        std::vector<fs::path> searched;
        fs::path found = find_game_root(searched);
        if (found.empty()) {
            LOG_E("boot", "could not find the game dump (a folder containing PS3_GAME\\USRDIR). Searched:");
            for (auto& s : searched) LOG_E("boot", "  %s", to_utf8(s).c_str());
            LOG_E("boot", "Pass it explicitly:  dante.exe --game \"C:\\path\\to\\Dante's Inferno\"");
            return 1;
        }
        game_root = to_utf8(found);
    }
    if (user_root.empty()) {
        // keep saves/caches next to the project, not wherever the exe was started from
        fs::path d = exe_dir();
        if (d.filename() == "build") d = d.parent_path();
        user_root = to_utf8(d / "userdata");
    }
    std::string elf = game_root + "/PS3_GAME/USRDIR/EBOOT.elf";
    std::error_code ec;
    if (!fs::exists(fs::path((const char8_t*)elf.c_str()), ec)) {
        LOG_E("boot", "%s not found.", elf.c_str());
        LOG_E("boot", "Decrypt PS3_GAME\\USRDIR\\EBOOT.BIN first: RPCS3 > Utilities > Decrypt PS3 Binaries.");
        return 1;
    }

    if (!vm::init()) {
        LOG_E("boot", "could not reserve the 4 GB guest address space");
        return 1;
    }
    vfs_init(game_root, user_root);
    if (!load_elf(elf)) return 1;

    // argv / envp for the guest: argv[0] is the boot path, envp is empty.
    uint32_t args = vm::alloc(vm::Area::Kernel, 0x1000);
    const char* boot_path = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
    vm::write_cstr(args + 0x100, boot_path);
    vm::write64(args + 0x00, args + 0x100);  // argv[0]
    vm::write64(args + 0x08, 0);             // argv[1] = NULL
    vm::write64(args + 0x10, 0);             // envp[0] = NULL

    uint32_t tid = thread_create(g_process.entry_opd, 0, (int)g_process.primary_prio, g_process.primary_stacksize, 0,
                                 "main_thread", true);
    PPCContext& ctx = *thread_context(tid);
    // Register state at process entry (as set up by lv2 / RPCS3's ppu_load_exec).
    ctx.r[3] = 1;            // argc
    ctx.r[4] = args;         // argv
    ctx.r[5] = args + 0x10;  // envp
    ctx.r[6] = 0;
    ctx.r[7] = tid;
    ctx.r[8] = g_process.tls_addr;
    ctx.r[9] = g_process.tls_filesz;
    ctx.r[10] = g_process.tls_memsz;
    ctx.r[11] = g_process.entry_opd;
    ctx.r[12] = g_process.malloc_pagesize;

    LOG_I("boot", "starting main thread 0x%X", tid);
    int code = run_main_thread(tid);
    LOG_I("boot", "main thread returned %d", code);
    process_exit(code);
}
