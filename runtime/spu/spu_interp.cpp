// SPU interpreter: decodes each instruction and runs its exec_<op> implementation (spu_ops.h).
// The same implementations are inlined into recompiled SPU code (tools/spu_recomp.py).
#include <cmath>
#include <cstring>
#include <map>

#include "spu.h"
#include "spu_ops.h"

SpuOp g_spu_decode[2048];
const char* g_spu_op_names[] = {
#define SPU_OP(name, fmt, first, count) #name,
#include "spu_opcodes.inc"
#undef SPU_OP
    "invalid"};

void spu_init_decoder() {
    static bool done = false;
    if (done) return;
    for (auto& e : g_spu_decode) e = SPU_INVALID;
#define SPU_OP(name, fmt, first, count) \
    for (int i = 0; i < count; ++i) g_spu_decode[first + i] = SPU_##name;
#include "spu_opcodes.inc"
#undef SPU_OP
    done = true;
}

// Debug aid: write LS + registers of a thread that stopped abnormally to spu_<id>_<pc>.{ls,regs}
void spu_dump_state(SpuThread& t, uint32_t pc, const char* why) {
    char fn[96];
    snprintf(fn, sizeof(fn), "spu_%08X_%05X.ls", t.id, pc);
    if (FILE* f = fopen(fn, "wb")) {
        fwrite(t.ls, 1, 0x40000, f);
        fclose(f);
    }
    snprintf(fn, sizeof(fn), "spu_%08X_%05X.regs", t.id, pc);
    if (FILE* f = fopen(fn, "w")) {
        fprintf(f, "%s at pc 0x%05X\n", why, pc);
        for (int i = 0; i < 128; ++i)
            fprintf(f, "$%-3d %08X %08X %08X %08X\n", i, VW(t.gpr[i], 0), VW(t.gpr[i], 1), VW(t.gpr[i], 2), VW(t.gpr[i], 3));
        fclose(f);
    }
    LOG_E("spu", "SPU 0x%X: %s at 0x%05X - local store and registers saved to spu_%08X_%05X.*", t.id, why, pc, t.id, pc);
}

std::string spu_disasm(uint32_t w, uint32_t pc) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%05X: %08X %s", pc, w, g_spu_op_names[g_spu_decode[w >> 21]]);
    return buf;
}


void spu_note_interp_page(SpuThread& t, uint32_t pc) {
    auto*& m = (std::map<uint64_t, uint32_t>*&)t.interp_pages;
    if (!m) m = new std::map<uint64_t, uint32_t>();
    ++(*m)[((uint64_t)(pc >> 12) << 32) | t.ls_src[(pc >> 12) & 63]];
    if (void* ov = t.ls_ov[(pc >> 12) & 63]) spu_note_overlay_exec(ov);
}

uint32_t spu_run(SpuThread& t) {
    spu_init_decoder();
    uint32_t pc = t.pc;
    for (;;) {
        if ((t.insn_count & 0xFFFF) == 0 && t.stop_req) {
            t.pc = pc;
            return 0;
        }
        ++t.insn_count;
        if (t.profile && !(t.insn_count & 255)) {
            ++t.profile[(pc >> 2) & 0xFFFF];
            spu_note_interp_page(t, pc);
        }
        uint32_t w = ppc_ld32(t.ls + (pc & 0x3FFFC));
        uint32_t npc = (pc + 4) & 0x3FFFC;
        uint64_t st;
        switch (g_spu_decode[w >> 21]) {
#define SPU_OP(name, fmt, first, count)     case SPU_##name: st = exec_##name(t, t.gpr, w, pc, npc); break;
#include "spu_opcodes.inc"
#undef SPU_OP
            default:
                spu_dump_state(t, pc, "invalid instruction");
                t.pc = pc;
                return 0xFFFFFFFD;
        }
        if (st) return (uint32_t)st;  // the op set t.pc where it matters
        if (npc != ((pc + 4) & 0x3FFFC)) {
            // taken branch into recompiled code: run natively until it leaves the translated region.
            // Native code returns when the target is outside its image (e.g. the kernel calling a
            // job), so try the other images before falling back to interpreting.
            while (spu_native_available(t, npc)) {
                uint64_t r = spu_run_native(t, npc);
                if (r) return (uint32_t)r;
                if (t.pc == npc) break;
                npc = t.pc;
            }
        }
        pc = npc;
    }
}
