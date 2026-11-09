// Dante's Inferno PC port - boot.
//
// Usage: dante.exe [--game <dump root>] [--trace]
//   <dump root> is the folder containing PS3_GAME/ (default: ../Dante's Inferno)
//   PS3_GAME/USRDIR/EBOOT.elf must be the RPCS3-decrypted executable.
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#include "kernel/kernel.h"

int main(int argc, char** argv) {
    std::string game_root = "../Dante's Inferno";
    std::string user_root = "userdata";
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

    if (!vm::init()) {
        LOG_E("boot", "could not reserve the 4 GB guest address space");
        return 1;
    }
    vfs_init(game_root, user_root);
    if (!load_elf(game_root + "/PS3_GAME/USRDIR/EBOOT.elf")) return 1;

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
