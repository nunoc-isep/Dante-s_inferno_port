// 128-byte line reservation semantics (lwarx/stwcx. vs other writers).
// Build: see tests/build_test.sh
#include <cstdio>
#include <cstdlib>

#include "ppc_context.h"

void ppc_line_touch(uint32_t ea, uint32_t size);

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

int main() {
    uint8_t* base = (uint8_t*)calloc(1, 0x10000);
    static PPCContext ctx;
    const uint32_t A = 0x1000;  // reserved word; A+0x40 is in the same 128-byte line, 0x1080 is not

    ppc_st32(base + A, 5);
    ppc_lwarx(ctx, base, A);
    check(ppc_stwcx(ctx, base, A, 6) && ppc_ld32(base + A) == 6, "undisturbed lwarx/stwcx. succeeds");

    ppc_lwarx(ctx, base, A);
    ppc_st32(base + A + 0x40, 0xAA);  // e.g. SPU PUT/PUTLLC to another word of the line
    ppc_line_touch(A + 0x40, 4);
    check(!ppc_stwcx(ctx, base, A, 7) && ppc_ld32(base + A) == 6, "write to another word in the line breaks it");

    ppc_lwarx(ctx, base, A);
    ppc_st32(base + A, 6);  // same value written back (ABA)
    ppc_line_touch(A, 4);
    check(!ppc_stwcx(ctx, base, A, 8), "ABA on the reserved word still breaks it");

    ppc_lwarx(ctx, base, A);
    ppc_line_touch(0x1080, 4);  // different line
    check(ppc_stwcx(ctx, base, A, 9) && ppc_ld32(base + A) == 9, "write to a different line does not");

    ppc_st64(base + 0x2000, 0x1122334455667788ull);
    ppc_ldarx(ctx, base, 0x2000);
    check(ppc_stdcx(ctx, base, 0x2000, 1) && ppc_ld64(base + 0x2000) == 1, "ldarx/stdcx. succeeds");

    printf(fails ? "%d atomic test(s) FAILED\n" : "all atomic tests passed\n", fails);
    return fails != 0;
}
