// Guest address space: one 4 GB host reservation, committed on demand, split
// into areas that mirror the PS3 user-space layout closely enough for the game.
#include <windows.h>

#include <cstdarg>
#include <map>
#include <mutex>

#include "kernel.h"

namespace vm {
uint8_t* g_base = nullptr;

struct AreaDesc {
    uint32_t base, size;
    std::map<uint32_t, uint32_t> free_blocks;  // start -> size
    std::map<uint32_t, uint32_t> used;         // start -> size
};

static AreaDesc g_areas[(int)Area::Count] = {
    {0x00010000, 0x1FFF0000},  // Main: ELF image (+ anything mapped there)
    {0x30000000, 0x10000000},  // User: sys_memory_allocate
    {0x20000000, 0x10000000},  // Kernel: HLE-owned objects (TLS, argv, ...)
    {0x40000000, 0x80000000},  // Mmapper: sys_mmapper / large mappings
    {0xC0000000, 0x10000000},  // Video: RSX local memory (256 MB)
    {0xD0000000, 0x10000000},  // Stack: PPU thread stacks
};
static std::mutex g_mutex;

bool init() {
    g_base = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_base) return false;
    for (auto& a : g_areas) a.free_blocks[a.base] = a.size;
    LOG_I("vm", "reserved 4 GB guest space at host %p", (void*)g_base);
    return true;
}

void commit(uint32_t addr, uint32_t size) {
    if (!size) return;
    uint64_t lo = addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc(g_base + lo, (size_t)(hi - lo), MEM_COMMIT, PAGE_READWRITE)) {
        LOG_E("vm", "commit failed at 0x%08X size 0x%X (error %lu)", addr, size, GetLastError());
        abort();
    }
}

static bool carve(AreaDesc& a, uint32_t start, uint32_t size) {
    // find the free block containing [start, start+size)
    auto it = a.free_blocks.upper_bound(start);
    if (it == a.free_blocks.begin()) return false;
    --it;
    uint32_t bs = it->first, bsz = it->second;
    if ((uint64_t)start + size > (uint64_t)bs + bsz) return false;
    a.free_blocks.erase(it);
    if (start > bs) a.free_blocks[bs] = start - bs;
    uint64_t end = (uint64_t)start + size, bend = (uint64_t)bs + bsz;
    if (end < bend) a.free_blocks[(uint32_t)end] = (uint32_t)(bend - end);
    a.used[start] = size;
    return true;
}

uint32_t alloc(Area area, uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> lk(g_mutex);
    AreaDesc& a = g_areas[(int)area];
    if (!align) align = 0x10;
    size = (size + 0xFFF) & ~0xFFFu;
    for (auto& [bs, bsz] : a.free_blocks) {
        uint64_t start = ((uint64_t)bs + align - 1) & ~(uint64_t)(align - 1);
        if (start + size <= (uint64_t)bs + bsz) {
            uint32_t s = (uint32_t)start;
            carve(a, s, size);
            commit(s, size);
            return s;
        }
    }
    LOG_E("vm", "out of guest memory in area %d (size 0x%X)", (int)area, size);
    return 0;
}

bool alloc_fixed(Area area, uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_mutex);
    AreaDesc& a = g_areas[(int)area];
    size = (size + 0xFFF) & ~0xFFFu;
    if (!carve(a, addr & ~0xFFFu, size)) return false;
    commit(addr, size);
    return true;
}

bool free(Area area, uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_mutex);
    AreaDesc& a = g_areas[(int)area];
    auto it = a.used.find(addr);
    if (it == a.used.end()) return false;
    uint32_t size = it->second;
    a.used.erase(it);
    // insert and coalesce
    auto ins = a.free_blocks.emplace(addr, size).first;
    auto next = std::next(ins);
    if (next != a.free_blocks.end() && ins->first + ins->second == next->first) {
        ins->second += next->second;
        a.free_blocks.erase(next);
    }
    if (ins != a.free_blocks.begin()) {
        auto prev = std::prev(ins);
        if (prev->first + prev->second == ins->first) {
            prev->second += ins->second;
            a.free_blocks.erase(ins);
        }
    }
    return true;
}

uint32_t area_base(Area area) { return g_areas[(int)area].base; }
uint32_t area_size(Area area) { return g_areas[(int)area].size; }

std::string read_cstr(uint32_t a, size_t max) {
    std::string s;
    if (!a) return s;
    for (size_t i = 0; i < max; ++i) {
        char c = (char)g_base[a + i];
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

void write_cstr(uint32_t a, const std::string& s) {
    memcpy(g_base + a, s.c_str(), s.size() + 1);
}
}  // namespace vm

// ---------------------------------------------------------------- logging
LogLevel g_log_level = LogLevel::Info;
static std::mutex g_log_mutex;

void log_msg(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (lvl > g_log_level) return;
    static const char* names[] = {"E", "W", "I", "T"};
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_log_mutex);
    fprintf(stderr, "[%s][%s] %s\n", names[(int)lvl], tag, buf);
    fflush(stderr);
}
