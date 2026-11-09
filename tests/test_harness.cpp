// Smoke test: load EBOOT.elf segments into a 4 GB guest address space and run
// recompiled game functions natively.
//
// Build (see tests/build_test.sh): needs the functions under test emitted
// with `recomp.py ... --only <addrs>` so the link is self-contained.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "ppc_context.h"
#include "ppc_funcs.h"

static uint8_t* g_base;

static uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    return (d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3];
}
static uint64_t rd64(const std::vector<uint8_t>& d, size_t o) {
    return ((uint64_t)rd32(d, o) << 32) | rd32(d, o + 4);
}
static uint16_t rd16(const std::vector<uint8_t>& d, size_t o) { return (d[o] << 8) | d[o + 1]; }

static void commit(uint32_t addr, uint32_t size) {
    uint32_t lo = addr & ~0xFFFFu, hi = (addr + size + 0xFFFF) & ~0xFFFFu;
    if (!VirtualAlloc(g_base + lo, hi - lo, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "commit failed at %08X\n", addr);
        exit(1);
    }
}

static bool load_elf(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    uint64_t phoff = rd64(d, 0x20);
    uint16_t phentsize = rd16(d, 0x36), phnum = rd16(d, 0x38);
    for (int i = 0; i < phnum; ++i) {
        size_t p = phoff + (size_t)i * phentsize;
        if (rd32(d, p) != 1) continue;  // PT_LOAD
        uint64_t off = rd64(d, p + 8), va = rd64(d, p + 16), filesz = rd64(d, p + 32), memsz = rd64(d, p + 40);
        if (!memsz) continue;
        commit((uint32_t)va, (uint32_t)memsz);
        memcpy(g_base + va, d.data() + off, filesz);
        printf("loaded segment va=%08llX size=%08llX\n", (unsigned long long)va, (unsigned long long)memsz);
    }
    return true;
}

static int g_fail = 0;
#define CHECK(cond, ...)                         \
    do {                                         \
        if (cond) {                              \
            printf("  PASS ");                   \
        } else {                                 \
            printf("  FAIL ");                   \
            ++g_fail;                            \
        }                                        \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <EBOOT.elf>\n", argv[0]);
        return 2;
    }
    g_base = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_base || !load_elf(argv[1])) {
        fprintf(stderr, "failed to set up guest memory / load ELF\n");
        return 1;
    }
    const uint32_t scratch = 0x30000000;
    commit(scratch, 0x10000);

    static PPCContext ctx;
    const char* samples[] = {"", "a", "Dante's Inferno", "Abandon all hope, ye who enter here"};

    printf("sub_0079E188 (strlen, returns zero-extended):\n");
    for (const char* s : samples) {
        memset(&ctx, 0, sizeof(ctx));
        strcpy((char*)g_base + scratch, s);
        ctx.r[3] = scratch;
        sub_0079E188(ctx, g_base);
        CHECK(ctx.r[3] == strlen(s), "\"%s\" -> %llu", s, (unsigned long long)ctx.r[3]);
    }
    printf("sub_00324B70 (strlen, returns sign-extended):\n");
    for (const char* s : samples) {
        memset(&ctx, 0, sizeof(ctx));
        strcpy((char*)g_base + scratch, s);
        ctx.r[3] = scratch;
        sub_00324B70(ctx, g_base);
        CHECK(ctx.r[3] == strlen(s), "\"%s\" -> %llu", s, (unsigned long long)ctx.r[3]);
    }
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
