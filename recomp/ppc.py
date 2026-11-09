"""PowerPC64 (Cell PPU) instruction -> C++ translator.

translate(word, addr, fn) returns (mnemonic, cpp_code) or (mnemonic, None)
when the instruction is not supported yet. `fn` is the FunctionContext that
answers branch questions (is target inside this function? is it an import?).

Conventions used by the emitted C++ (see runtime/ppc_context.h):
  ctx.r[n] u64 GPRs, ctx.f[n] FPR union (.f64/.u64), ctx.cr[n] CR fields,
  ctx.lr / ctx.ctr u64, ctx.xer.{so,ov,ca}, base = guest memory base.
  Memory macros byte-swap: PPC_LOAD_U32(ea), PPC_STORE_U32(ea, v), ...
"""

MASK64_ALL = (1 << 64) - 1


def mask64(mb, me):
    """IBM bit numbering mask, bits mb..me set (wraps when mb > me)."""
    a = MASK64_ALL >> mb
    b = (MASK64_ALL << (63 - me)) & MASK64_ALL
    return (a & b) if mb <= me else (a | b)


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def hx(v):
    return f"0x{v & MASK64_ALL:X}ull"


def r(n):
    return f"ctx.r[{n}]"


def f(n):
    return f"ctx.f[{n}]"


def ra0(n):
    """(rA|0) addressing operand."""
    return "0" if n == 0 else r(n)


def cr0(res):
    return f"ctx.cr[0].compare<int64_t>((int64_t){res}, 0, ctx.xer.so);"


CR_BITS = ["lt", "gt", "eq", "so"]


def crbit(bi):
    return f"ctx.cr[{bi >> 2}].{CR_BITS[bi & 3]}"


class Insn:
    __slots__ = ("w", "addr")

    def __init__(self, w, addr):
        self.w, self.addr = w, addr

    opcd = property(lambda s: s.w >> 26)
    rd = property(lambda s: (s.w >> 21) & 31)
    rs = rd
    ra = property(lambda s: (s.w >> 16) & 31)
    rb = property(lambda s: (s.w >> 11) & 31)
    rc_reg = property(lambda s: (s.w >> 6) & 31)
    simm = property(lambda s: sext16(s.w & 0xFFFF))
    uimm = property(lambda s: s.w & 0xFFFF)
    rc = property(lambda s: s.w & 1)
    xo10 = property(lambda s: (s.w >> 1) & 0x3FF)
    xo9 = property(lambda s: (s.w >> 1) & 0x1FF)
    xo5 = property(lambda s: (s.w >> 1) & 0x1F)


# ---------------------------------------------------------------- branches

def bc_cond(bo, bi):
    """Return (pre_statements, condition_expr) for a BO/BI pair."""
    pre, conds = "", []
    if not bo & 4:
        pre = "--ctx.ctr; "
        conds.append("(uint32_t)ctx.ctr == 0" if bo & 2 else "(uint32_t)ctx.ctr != 0")
    if not bo & 16:
        conds.append(crbit(bi) if bo & 8 else f"!{crbit(bi)}")
    return pre, " && ".join(conds) if conds else "true"


def emit_branch_to(target, fn, link, addr):
    """Unconditional transfer to a direct target."""
    if link:
        return f"ctx.lr = 0x{addr + 4:X}; {fn.call(target)}"
    if fn.contains(target):
        return f"goto loc_{target:08X};"
    return f"{fn.call(target)} return;"  # tail call


def t_branch(i, fn):
    w = i.w
    if i.opcd == 18:  # b / bl / ba
        li = sext(w & 0x03FFFFFC, 26)
        target = (li if w & 2 else i.addr + li) & 0xFFFFFFFF
        return ("bl" if w & 1 else "b"), emit_branch_to(target, fn, w & 1, i.addr)
    if i.opcd == 16:  # bc
        bo, bi = i.rd, i.ra
        bd = sext(w & 0xFFFC, 16)
        target = (bd if w & 2 else i.addr + bd) & 0xFFFFFFFF
        pre, cond = bc_cond(bo, bi)
        body = emit_branch_to(target, fn, w & 1, i.addr)
        return ("bcl" if w & 1 else "bc"), f"{pre}if ({cond}) {{ {body} }}"
    return None


def t_op19(i, fn):
    xo = i.xo10
    if xo == 16:  # bclr
        pre, cond = bc_cond(i.rd, i.ra)
        if i.rc:  # bclrl: call through LR
            body = f"{{ uint64_t t = ctx.lr; ctx.lr = 0x{i.addr + 4:X}; PPC_CALL_INDIRECT(t); }}"
            return "bclrl", f"{pre}if ({cond}) {body}"
        if cond == "true" and not pre:
            return "blr", "return;"
        return "bclr", f"{pre}if ({cond}) return;"
    if xo == 528:  # bcctr
        pre, cond = bc_cond(i.rd | 4, i.ra)  # CTR is never decremented
        if i.rc:
            return "bcctrl", f"if ({cond}) {{ ctx.lr = 0x{i.addr + 4:X}; PPC_CALL_INDIRECT(ctx.ctr); }}"
        jt = fn.jump_table(i.addr)
        if jt is not None:
            cases = " ".join(f"case 0x{t:X}: goto loc_{t:08X};" for t in sorted(set(jt)))
            return "bcctr", (f"if ({cond}) {{ switch ((uint32_t)ctx.ctr) {{ {cases} "
                             f"default: PPC_UNREACHABLE(0x{i.addr:X}); }} }}")
        return "bcctr", f"if ({cond}) {{ PPC_CALL_INDIRECT(ctx.ctr); return; }}"
    if xo == 0:  # mcrf
        return "mcrf", f"ctx.cr[{i.rd >> 2}] = ctx.cr[{i.ra >> 2}];"
    if xo == 150:
        return "isync", ";"
    crops = {257: "{a} && {b}", 449: "{a} || {b}", 193: "{a} != {b}", 225: "!({a} && {b})",
             33: "!({a} || {b})", 289: "{a} == {b}", 129: "{a} && !{b}", 417: "{a} || !{b}"}
    names = {257: "crand", 449: "cror", 193: "crxor", 225: "crnand", 33: "crnor",
             289: "creqv", 129: "crandc", 417: "crorc"}
    if xo in crops:
        expr = crops[xo].format(a=crbit(i.ra), b=crbit(i.rb))
        return names[xo], f"{crbit(i.rd)} = {expr};"
    return None


# ---------------------------------------------------------------- integer

def t_dform(i, fn):
    o, d, a, s = i.opcd, i.rd, i.ra, i.simm
    u = i.uimm
    if o == 14:  # addi / li
        return ("li", f"{r(d)} = {hx(s)};") if a == 0 else ("addi", f"{r(d)} = {r(a)} + {hx(s)};")
    if o == 15:  # addis / lis
        v = s << 16
        return ("lis", f"{r(d)} = {hx(v)};") if a == 0 else ("addis", f"{r(d)} = {r(a)} + {hx(v)};")
    if o in (12, 13):  # addic, addic.
        code = (f"{{ uint64_t a = {r(a)}; uint64_t t = a + {hx(s)}; ctx.xer.ca = t < a; {r(d)} = t; }}")
        if o == 13:
            code += " " + cr0(r(d))
        return ("addic." if o == 13 else "addic"), code
    if o == 8:  # subfic
        return "subfic", (f"{{ uint64_t a = {r(a)}; ctx.xer.ca = a <= {hx(s)}; "
                          f"{r(d)} = {hx(s)} - a; }}")
    if o == 7:
        return "mulli", f"{r(d)} = (uint64_t)((int64_t){r(a)} * {s});"
    if o in (10, 11):  # cmpli / cmpi
        crf, l64 = d >> 2, d & 1
        if o == 11:
            ty = "int64_t" if l64 else "int32_t"
            return "cmpi", f"ctx.cr[{crf}].compare<{ty}>(({ty}){r(a)}, {s}, ctx.xer.so);"
        ty = "uint64_t" if l64 else "uint32_t"
        return "cmpli", f"ctx.cr[{crf}].compare<{ty}>(({ty}){r(a)}, {u}u, ctx.xer.so);"
    # logical immediate: rS is in the rd slot, destination rA
    if o == 24:
        if d == 0 and a == 0 and u == 0:
            return "nop", ";"
        return "ori", f"{r(a)} = {r(d)} | {u}u;"
    if o == 25:
        return "oris", f"{r(a)} = {r(d)} | {hx(u << 16)};"
    if o == 26:
        return "xori", f"{r(a)} = {r(d)} ^ {u}u;"
    if o == 27:
        return "xoris", f"{r(a)} = {r(d)} ^ {hx(u << 16)};"
    if o == 28:
        return "andi.", f"{r(a)} = {r(d)} & {u}u; {cr0(r(a))}"
    if o == 29:
        return "andis.", f"{r(a)} = {r(d)} & {hx(u << 16)}; {cr0(r(a))}"
    return None


def t_rotate32(i, fn):
    s, a = i.rs, i.ra
    sh, mb, me = i.rb, (i.w >> 6) & 31, (i.w >> 1) & 31
    m = hx(mask64(mb + 32, me + 32))
    rot = lambda amt: f"PPC_ROTL32_DUP({r(s)}, {amt})"
    if i.opcd == 21:
        code, name = f"{r(a)} = {rot(sh)} & {m};", "rlwinm"
    elif i.opcd == 20:
        code, name = f"{r(a)} = ({rot(sh)} & {m}) | ({r(a)} & ~{m});", "rlwimi"
    else:
        code, name = f"{r(a)} = {rot(f'{r(i.rb)} & 31')} & {m};", "rlwnm"
    if i.rc:
        code += " " + cr0(r(a))
        name += "."
    return name, code


def t_rotate64(i, fn):
    w, s, a = i.w, i.rs, i.ra
    sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
    mbe = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
    x3 = (w >> 2) & 7
    rot = f"PPC_ROTL64({r(s)}, {sh})"
    if x3 == 0:
        name, code = "rldicl", f"{r(a)} = {rot} & {hx(mask64(mbe, 63))};"
    elif x3 == 1:
        name, code = "rldicr", f"{r(a)} = {rot} & {hx(mask64(0, mbe))};"
    elif x3 == 2:
        name, code = "rldic", f"{r(a)} = {rot} & {hx(mask64(mbe, 63 - sh))};"
    elif x3 == 3:
        m = hx(mask64(mbe, 63 - sh))
        name, code = "rldimi", f"{r(a)} = ({rot} & {m}) | ({r(a)} & ~{m});"
    else:
        x4 = (w >> 1) & 15
        rot = f"PPC_ROTL64({r(s)}, {r(i.rb)} & 63)"
        if x4 == 8:
            name, code = "rldcl", f"{r(a)} = {rot} & {hx(mask64(mbe, 63))};"
        elif x4 == 9:
            name, code = "rldcr", f"{r(a)} = {rot} & {hx(mask64(0, mbe))};"
        else:
            return None
    if i.rc:
        code += " " + cr0(r(a))
        name += "."
    return name, code


ARITH = {  # xo9: (name, expression using a, b, ca)  -- result into rD
    266: ("add", "a + b", None),
    40: ("subf", "b - a", None),
    104: ("neg", "0 - a", None),
    235: ("mullw", "(uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b)", None),
    233: ("mulld", "a * b", None),
    75: ("mulhw", "(uint64_t)(int64_t)(int32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32)", None),
    11: ("mulhwu", "(uint64_t)(uint32_t)(((uint64_t)(uint32_t)a * (uint32_t)b) >> 32)", None),
    73: ("mulhd", "(uint64_t)PPC_MULH_S64(a, b)", None),
    9: ("mulhdu", "PPC_MULH_U64(a, b)", None),
    491: ("divw", "PPC_DIVW(a, b)", None),
    459: ("divwu", "((uint32_t)b ? (uint64_t)((uint32_t)a / (uint32_t)b) : 0)", None),
    489: ("divd", "PPC_DIVD(a, b)", None),
    457: ("divdu", "(b ? a / b : 0)", None),
    # carrying forms: (name, result, carry-out)
    10: ("addc", "a + b", "t < a"),
    8: ("subfc", "b - a", "a <= b"),
    138: ("adde", "a + b + ca", "PPC_CARRY3(a, b, ca)"),
    136: ("subfe", "~a + b + ca", "PPC_CARRY3(~a, b, ca)"),
    202: ("addze", "a + ca", "t < a"),
    200: ("subfze", "~a + ca", "PPC_CARRY3(~a, 0, ca)"),
    234: ("addme", "a + ca - 1", "PPC_CARRY3(a, ~0ull, ca)"),
    232: ("subfme", "~a + ca - 1", "PPC_CARRY3(~a, ~0ull, ca)"),
}


def t_op31(i, fn):
    xo, xo9 = i.xo10, i.xo9
    d, a, b = i.rd, i.ra, i.rb
    s = d  # for X-form logical/store ops the rS register sits in the rd slot

    if xo in VMX_MEM31:
        return t_vmx_mem(i)

    if xo9 in ARITH and not (xo9 == 75 and xo == 75 + 512):  # OE bit lives above xo9
        name, expr, carry = ARITH[xo9]
        code = f"{{ uint64_t a = {r(a)}, b = {r(b)}; uint64_t ca = ctx.xer.ca; (void)b; (void)ca; uint64_t t = {expr};"
        if carry:
            code += f" ctx.xer.ca = {carry};"
        code += f" {r(d)} = t; }}"
        if (i.w >> 10) & 1:
            name += "o"  # overflow tracking not implemented; result still correct
        if i.rc:
            code += " " + cr0(r(d))
            name += "."
        return name, code

    if xo in (0, 32):  # cmp / cmpl
        crf, l64 = d >> 2, d & 1
        if xo == 0:
            ty = "int64_t" if l64 else "int32_t"
        else:
            ty = "uint64_t" if l64 else "uint32_t"
        return ("cmp" if xo == 0 else "cmpl"), f"ctx.cr[{crf}].compare<{ty}>(({ty}){r(a)}, ({ty}){r(b)}, ctx.xer.so);"

    logic = {28: ("and", "{s} & {b}"), 60: ("andc", "{s} & ~{b}"), 444: ("or", "{s} | {b}"),
             412: ("orc", "{s} | ~{b}"), 124: ("nor", "~({s} | {b})"), 316: ("xor", "{s} ^ {b}"),
             284: ("eqv", "~({s} ^ {b})"), 476: ("nand", "~({s} & {b})")}
    if xo in logic:
        name, e = logic[xo]
        if xo == 444 and s == b:
            name, e = "mr", "{s}"
        code = f"{r(a)} = {e.format(s=r(s), b=r(b))};"
        if i.rc:
            code += " " + cr0(r(a))
            name += "."
        return name, code

    unary = {26: ("cntlzw", "PPC_CNTLZ32((uint32_t){s})"), 58: ("cntlzd", "PPC_CNTLZ64({s})"),
             954: ("extsb", "(uint64_t)(int64_t)(int8_t){s}"),
             922: ("extsh", "(uint64_t)(int64_t)(int16_t){s}"),
             986: ("extsw", "(uint64_t)(int64_t)(int32_t){s}")}
    if xo in unary:
        name, e = unary[xo]
        code = f"{r(a)} = {e.format(s=r(s))};"
        if i.rc:
            code += " " + cr0(r(a))
            name += "."
        return name, code

    shifts = {
        24: ("slw", "{{ uint64_t n = {b} & 63; {a} = n > 31 ? 0 : (uint64_t)(uint32_t)((uint32_t){s} << n); }}"),
        536: ("srw", "{{ uint64_t n = {b} & 63; {a} = n > 31 ? 0 : (uint64_t)((uint32_t){s} >> n); }}"),
        27: ("sld", "{{ uint64_t n = {b} & 127; {a} = n > 63 ? 0 : {s} << n; }}"),
        539: ("srd", "{{ uint64_t n = {b} & 127; {a} = n > 63 ? 0 : {s} >> n; }}"),
        792: ("sraw", "{{ int32_t v = (int32_t){s}; uint64_t n = {b} & 63; if (n > 31) {{ ctx.xer.ca = v < 0; {a} = (uint64_t)(int64_t)(v >> 31); }} else {{ ctx.xer.ca = v < 0 && n && (v & ((1u << n) - 1)); {a} = (uint64_t)(int64_t)(v >> n); }} }}"),
        794: ("srad", "{{ int64_t v = (int64_t){s}; uint64_t n = {b} & 127; if (n > 63) {{ ctx.xer.ca = v < 0; {a} = (uint64_t)(v >> 63); }} else {{ ctx.xer.ca = v < 0 && n && (v & ((1ull << n) - 1)); {a} = (uint64_t)(v >> n); }} }}"),
    }
    if xo in shifts:
        name, e = shifts[xo]
        code = e.format(a=r(a), b=r(b), s=r(s))
        if i.rc:
            code += " " + cr0(r(a))
            name += "."
        return name, code
    if xo == 824:  # srawi
        n = b
        mask = (1 << n) - 1
        code = (f"{{ int32_t v = (int32_t){r(s)}; ctx.xer.ca = v < 0 && (v & {mask}u); "
                f"{r(a)} = (uint64_t)(int64_t)(v >> {n}); }}")
        if i.rc:
            code += " " + cr0(r(a))
        return ("srawi." if i.rc else "srawi"), code
    if (i.w >> 2) & 0x1FF == 413:  # sradi
        n = b | (((i.w >> 1) & 1) << 5)
        mask = (1 << n) - 1
        code = (f"{{ int64_t v = (int64_t){r(s)}; ctx.xer.ca = v < 0 && (v & {hx(mask)}); "
                f"{r(a)} = (uint64_t)(v >> {n}); }}")
        if i.rc:
            code += " " + cr0(r(a))
        return ("sradi." if i.rc else "sradi"), code

    # special registers
    if xo in (339, 467, 371):
        spr = ((i.w >> 16) & 31) | (((i.w >> 11) & 31) << 5)
        if xo == 371 or (xo == 339 and spr in (268, 269)):
            return "mftb", f"{r(d)} = PPC_MFTB();"
        if xo == 339:
            src = {8: "ctx.lr", 9: "ctx.ctr", 1: "ctx.xer.get()", 256: "ctx.vrsave"}.get(spr)
            return (f"mfspr{spr}", f"{r(d)} = {src};") if src else None
        dst = {8: "ctx.lr = {v};", 9: "ctx.ctr = {v};", 1: "ctx.xer.set({v});",
               256: "ctx.vrsave = (uint32_t){v};"}.get(spr)
        return (f"mtspr{spr}", dst.format(v=r(s))) if dst else None
    if xo == 19:
        return "mfcr", f"{r(d)} = ctx.get_cr();"
    if xo == 144:
        fxm = (i.w >> 12) & 0xFF
        return "mtcrf", f"ctx.set_cr((uint32_t){r(s)}, 0x{fxm:02X});"

    # indexed loads / stores
    ea = f"({ra0(a)} + {r(b)})"
    upd = lambda: f" {r(a)} = ea;"
    loads = {23: ("lwzx", "PPC_LOAD_U32(ea)", 0), 55: ("lwzux", "PPC_LOAD_U32(ea)", 1),
             87: ("lbzx", "PPC_LOAD_U8(ea)", 0), 119: ("lbzux", "PPC_LOAD_U8(ea)", 1),
             279: ("lhzx", "PPC_LOAD_U16(ea)", 0), 311: ("lhzux", "PPC_LOAD_U16(ea)", 1),
             343: ("lhax", "(uint64_t)(int64_t)(int16_t)PPC_LOAD_U16(ea)", 0),
             375: ("lhaux", "(uint64_t)(int64_t)(int16_t)PPC_LOAD_U16(ea)", 1),
             21: ("ldx", "PPC_LOAD_U64(ea)", 0), 53: ("ldux", "PPC_LOAD_U64(ea)", 1),
             341: ("lwax", "(uint64_t)(int64_t)(int32_t)PPC_LOAD_U32(ea)", 0),
             373: ("lwaux", "(uint64_t)(int64_t)(int32_t)PPC_LOAD_U32(ea)", 1),
             534: ("lwbrx", "PPC_LOAD_U32_LE(ea)", 0), 790: ("lhbrx", "PPC_LOAD_U16_LE(ea)", 0),
             532: ("ldbrx", "PPC_LOAD_U64_LE(ea)", 0)}
    if xo in loads:
        name, e, u = loads[xo]
        return name, f"{{ uint64_t ea = {ea}; {r(d)} = {e};{upd() if u else ''} }}"
    stores = {151: ("stwx", "PPC_STORE_U32", 0), 183: ("stwux", "PPC_STORE_U32", 1),
              215: ("stbx", "PPC_STORE_U8", 0), 247: ("stbux", "PPC_STORE_U8", 1),
              407: ("sthx", "PPC_STORE_U16", 0), 439: ("sthux", "PPC_STORE_U16", 1),
              149: ("stdx", "PPC_STORE_U64", 0), 181: ("stdux", "PPC_STORE_U64", 1),
              662: ("stwbrx", "PPC_STORE_U32_LE", 0), 918: ("sthbrx", "PPC_STORE_U16_LE", 0),
              660: ("stdbrx", "PPC_STORE_U64_LE", 0)}
    if xo in stores:
        name, m, u = stores[xo]
        return name, f"{{ uint64_t ea = {ea}; {m}(ea, {r(s)});{upd() if u else ''} }}"
    fl = {535: ("lfsx", "s", 0), 567: ("lfsux", "s", 1), 599: ("lfdx", "d", 0), 631: ("lfdux", "d", 1)}
    if xo in fl:
        name, k, u = fl[xo]
        e = "ctx.f[{d}].f64 = (double)PPC_LOAD_F32(ea);" if k == "s" else "ctx.f[{d}].u64 = PPC_LOAD_U64(ea);"
        return name, f"{{ uint64_t ea = {ea}; {e.format(d=d)}{upd() if u else ''} }}"
    fs = {663: ("stfsx", "s", 0), 695: ("stfsux", "s", 1), 727: ("stfdx", "d", 0),
          759: ("stfdux", "d", 1), 983: ("stfiwx", "i", 0)}
    if xo in fs:
        name, k, u = fs[xo]
        e = {"s": "PPC_STORE_F32(ea, (float)ctx.f[{s}].f64);",
             "d": "PPC_STORE_U64(ea, ctx.f[{s}].u64);",
             "i": "PPC_STORE_U32(ea, (uint32_t)ctx.f[{s}].u64);"}[k]
        return name, f"{{ uint64_t ea = {ea}; {e.format(s=s)}{upd() if u else ''} }}"

    # atomics
    if xo == 20:
        return "lwarx", f"{{ uint64_t ea = {ea}; {r(d)} = PPC_LWARX(ea); }}"
    if xo == 84:
        return "ldarx", f"{{ uint64_t ea = {ea}; {r(d)} = PPC_LDARX(ea); }}"
    if xo == 150:
        return "stwcx.", f"{{ uint64_t ea = {ea}; ctx.cr[0].set_stcx(PPC_STWCX(ea, {r(s)}), ctx.xer.so); }}"
    if xo == 214:
        return "stdcx.", f"{{ uint64_t ea = {ea}; ctx.cr[0].set_stcx(PPC_STDCX(ea, {r(s)}), ctx.xer.so); }}"

    # cache / sync / traps
    if xo == 1014:
        return "dcbz", f"PPC_DCBZ({ea});"
    if xo in (598, 854):
        return ("sync" if xo == 598 else "eieio"), "PPC_FENCE();"
    if xo in (278, 246, 86, 54, 982, 22):
        return {278: "dcbt", 246: "dcbtst", 86: "dcbf", 54: "dcbst", 982: "icbi", 22: "icbt"}[xo], ";"
    if xo in (4, 68):
        to = d
        name = "tw" if xo == 4 else "td"
        if to == 31:
            return name, f"PPC_TRAP(0x{i.addr:X});"
        ty = "int32_t" if xo == 4 else "int64_t"
        uty = "uint32_t" if xo == 4 else "uint64_t"
        conds = []
        A, B = f"({ty}){r(a)}", f"({ty}){r(b)}"
        uA, uB = f"({uty}){r(a)}", f"({uty}){r(b)}"
        if to & 16: conds.append(f"{A} < {B}")
        if to & 8: conds.append(f"{A} > {B}")
        if to & 4: conds.append(f"{A} == {B}")
        if to & 2: conds.append(f"{uA} < {uB}")
        if to & 1: conds.append(f"{uA} > {uB}")
        return name, f"if ({' || '.join(conds) or 'false'}) PPC_TRAP(0x{i.addr:X});"
    return None


def t_trap_imm(i, fn):
    to, a, s = i.rd, i.ra, i.simm
    name = "twi" if i.opcd == 3 else "tdi"
    if to == 31:
        return name, f"PPC_TRAP(0x{i.addr:X});"
    ty, uty = ("int32_t", "uint32_t") if i.opcd == 3 else ("int64_t", "uint64_t")
    A, uA = f"({ty}){r(a)}", f"({uty}){r(a)}"
    B, uB = f"({ty}){s}", f"({uty})({ty}){s}"
    conds = []
    if to & 16: conds.append(f"{A} < {B}")
    if to & 8: conds.append(f"{A} > {B}")
    if to & 4: conds.append(f"{A} == {B}")
    if to & 2: conds.append(f"{uA} < {uB}")
    if to & 1: conds.append(f"{uA} > {uB}")
    return name, f"if ({' || '.join(conds) or 'false'}) PPC_TRAP(0x{i.addr:X});"


# ---------------------------------------------------------------- loads/stores (D/DS form)

def t_mem(i, fn):
    o, d, a = i.opcd, i.rd, i.ra
    disp = i.simm
    if o in (58, 62):  # DS form
        disp = sext16(i.w & 0xFFFC)
        x = i.w & 3
        ea = f"({ra0(a)} + {hx(disp)})"
        if o == 58:
            m = {0: ("ld", "PPC_LOAD_U64(ea)", 0), 1: ("ldu", "PPC_LOAD_U64(ea)", 1),
                 2: ("lwa", "(uint64_t)(int64_t)(int32_t)PPC_LOAD_U32(ea)", 0)}.get(x)
            if not m:
                return None
            name, e, u = m
            return name, f"{{ uint64_t ea = {ea}; {r(d)} = {e};{f' {r(a)} = ea;' if u else ''} }}"
        m = {0: ("std", 0), 1: ("stdu", 1)}.get(x)
        if not m:
            return None
        name, u = m
        return name, f"{{ uint64_t ea = {ea}; PPC_STORE_U64(ea, {r(d)});{f' {r(a)} = ea;' if u else ''} }}"

    ea = f"({ra0(a)} + {hx(disp)})"
    upd = f" {r(a)} = ea;"
    loads = {32: ("lwz", "PPC_LOAD_U32(ea)"), 33: ("lwzu", "PPC_LOAD_U32(ea)"),
             34: ("lbz", "PPC_LOAD_U8(ea)"), 35: ("lbzu", "PPC_LOAD_U8(ea)"),
             40: ("lhz", "PPC_LOAD_U16(ea)"), 41: ("lhzu", "PPC_LOAD_U16(ea)"),
             42: ("lha", "(uint64_t)(int64_t)(int16_t)PPC_LOAD_U16(ea)"),
             43: ("lhau", "(uint64_t)(int64_t)(int16_t)PPC_LOAD_U16(ea)")}
    if o in loads:
        name, e = loads[o]
        return name, f"{{ uint64_t ea = {ea}; {r(d)} = {e};{upd if o & 1 else ''} }}"
    stores = {36: ("stw", "PPC_STORE_U32"), 37: ("stwu", "PPC_STORE_U32"),
              38: ("stb", "PPC_STORE_U8"), 39: ("stbu", "PPC_STORE_U8"),
              44: ("sth", "PPC_STORE_U16"), 45: ("sthu", "PPC_STORE_U16")}
    if o in stores:
        name, m = stores[o]
        return name, f"{{ uint64_t ea = {ea}; {m}(ea, {r(d)});{upd if o & 1 else ''} }}"
    if o == 46:  # lmw
        return "lmw", (f"{{ uint64_t ea = {ea}; for (int k = {d}; k < 32; ++k, ea += 4) "
                       f"ctx.r[k] = PPC_LOAD_U32(ea); }}")
    if o == 47:  # stmw
        return "stmw", (f"{{ uint64_t ea = {ea}; for (int k = {d}; k < 32; ++k, ea += 4) "
                        f"PPC_STORE_U32(ea, ctx.r[k]); }}")
    fl = {48: ("lfs", "ctx.f[{d}].f64 = (double)PPC_LOAD_F32(ea);"),
          49: ("lfsu", "ctx.f[{d}].f64 = (double)PPC_LOAD_F32(ea);"),
          50: ("lfd", "ctx.f[{d}].u64 = PPC_LOAD_U64(ea);"),
          51: ("lfdu", "ctx.f[{d}].u64 = PPC_LOAD_U64(ea);"),
          52: ("stfs", "PPC_STORE_F32(ea, (float)ctx.f[{d}].f64);"),
          53: ("stfsu", "PPC_STORE_F32(ea, (float)ctx.f[{d}].f64);"),
          54: ("stfd", "PPC_STORE_U64(ea, ctx.f[{d}].u64);"),
          55: ("stfdu", "PPC_STORE_U64(ea, ctx.f[{d}].u64);")}
    if o in fl:
        name, e = fl[o]
        return name, f"{{ uint64_t ea = {ea}; {e.format(d=d)}{upd if o & 1 else ''} }}"
    return None


# ---------------------------------------------------------------- floating point

def t_fp(i, fn):
    o, d, a, b, c = i.opcd, i.rd, i.ra, i.rb, i.rc_reg
    single = o == 59
    rnd = (lambda e: f"(double)(float)({e})") if single else (lambda e: e)
    sfx = "s" if single else ""
    A, B, C = f"{f(a)}.f64", f"{f(b)}.f64", f"{f(c)}.f64"
    x5 = i.xo5
    aform = {21: ("fadd", f"{A} + {B}"), 20: ("fsub", f"{A} - {B}"), 25: ("fmul", f"{A} * {C}"),
             18: ("fdiv", f"{A} / {B}"), 29: ("fmadd", f"PPC_FMA({A}, {C}, {B})"),
             28: ("fmsub", f"PPC_FMA({A}, {C}, -{B})"), 31: ("fnmadd", f"-PPC_FMA({A}, {C}, {B})"),
             30: ("fnmsub", f"-PPC_FMA({A}, {C}, -{B})"), 22: ("fsqrt", f"PPC_SQRT({B})"),
             24: ("fres", f"1.0 / {B}"), 26: ("frsqrte", f"1.0 / PPC_SQRT({B})"),
             23: ("fsel", f"({A} >= 0.0 ? {C} : {B})")}
    if x5 in aform and (o == 59 or x5 in (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31)):
        name, e = aform[x5]
        if x5 == 23:
            return "fsel", f"{f(d)}.f64 = {e};"
        name += sfx
        code = f"{f(d)}.f64 = {rnd(e)};"
        if i.rc:
            code += " ctx.update_cr1();"
            name += "."
        return name, code
    if o == 59:
        return None
    xo = i.xo10
    if xo in (0, 32):
        return ("fcmpu" if xo == 0 else "fcmpo"), f"ctx.cr[{d >> 2}].compare_f({A}, {B});"
    simple = {72: ("fmr", f"{f(d)}.u64 = {f(b)}.u64;"),
              40: ("fneg", f"{f(d)}.u64 = {f(b)}.u64 ^ 0x8000000000000000ull;"),
              264: ("fabs", f"{f(d)}.u64 = {f(b)}.u64 & 0x7FFFFFFFFFFFFFFFull;"),
              136: ("fnabs", f"{f(d)}.u64 = {f(b)}.u64 | 0x8000000000000000ull;"),
              12: ("frsp", f"{f(d)}.f64 = (double)(float){B};"),
              14: ("fctiw", f"{f(d)}.u64 = (uint64_t)(int64_t)PPC_F2I32_RN({B});"),
              15: ("fctiwz", f"{f(d)}.u64 = (uint64_t)(int64_t)PPC_F2I32({B});"),
              814: ("fctid", f"{f(d)}.u64 = (uint64_t)PPC_F2I64_RN({B});"),
              815: ("fctidz", f"{f(d)}.u64 = (uint64_t)PPC_F2I64({B});"),
              846: ("fcfid", f"{f(d)}.f64 = (double)(int64_t){f(b)}.u64;"),
              583: ("mffs", f"{f(d)}.u64 = ctx.fpscr;"),
              711: ("mtfsf", f"ctx.set_fpscr((uint32_t){f(b)}.u64, 0x{(i.w >> 17) & 0xFF:02X});"),
              134: ("mtfsfi", f"ctx.set_fpscr_field({d >> 2}, {(i.w >> 12) & 15});"),
              70: ("mtfsb0", f"ctx.fpscr &= ~(0x80000000u >> {d});"),
              38: ("mtfsb1", f"ctx.fpscr |= (0x80000000u >> {d});")}
    if xo in simple:
        name, code = simple[xo]
        if i.rc:
            code += " ctx.update_cr1();"
            name += "."
        return name, code
    return None


# ---------------------------------------------------------------- VMX / AltiVec

def v(n):
    return f"ctx.v[{n}]"


VMX_VA = {  # xo6: (name, argument order using a/b/c)
    42: ("vsel", "abc"), 43: ("vperm", "abc"), 46: ("vmaddfp", "acb"), 47: ("vnmsubfp", "acb"),
    34: ("vmladduhm", "abc"), 36: ("vmsumubm", "abc"), 38: ("vmsumuhm", "abc"), 40: ("vmsumshm", "abc"),
}

VMX_CMP = {6: "vcmpequb", 70: "vcmpequh", 134: "vcmpequw", 198: "vcmpeqfp", 454: "vcmpgefp",
           518: "vcmpgtub", 582: "vcmpgtuh", 646: "vcmpgtuw", 710: "vcmpgtfp", 774: "vcmpgtsb",
           838: "vcmpgtsh", 902: "vcmpgtsw", 966: "vcmpbfp"}

# xo11: (name, kind)  kinds: ab, b, cab (ctx,a,b), buimm, cbuimm (ctx,b,uimm), simm
VMX_VX = {
    10: ("vaddfp", "ab"), 74: ("vsubfp", "ab"), 1034: ("vmaxfp", "ab"), 1098: ("vminfp", "ab"),
    266: ("vrefp", "b"), 330: ("vrsqrtefp", "b"), 394: ("vexptefp", "b"), 458: ("vlogefp", "b"),
    522: ("vrfin", "b"), 586: ("vrfiz", "b"), 650: ("vrfip", "b"), 714: ("vrfim", "b"),
    778: ("vcfux", "buimm"), 842: ("vcfsx", "buimm"), 906: ("vctuxs", "cbuimm"), 970: ("vctsxs", "cbuimm"),
    0: ("vaddubm", "ab"), 64: ("vadduhm", "ab"), 128: ("vadduwm", "ab"),
    512: ("vaddubs", "ab"), 576: ("vadduhs", "ab"), 640: ("vadduws", "ab"),
    768: ("vaddsbs", "ab"), 832: ("vaddshs", "ab"), 896: ("vaddsws", "ab"), 384: ("vaddcuw", "ab"),
    1024: ("vsububm", "ab"), 1088: ("vsubuhm", "ab"), 1152: ("vsubuwm", "ab"),
    1536: ("vsububs", "ab"), 1600: ("vsubuhs", "ab"), 1664: ("vsubuws", "ab"),
    1792: ("vsubsbs", "ab"), 1856: ("vsubshs", "ab"), 1920: ("vsubsws", "ab"), 1408: ("vsubcuw", "ab"),
    2: ("vmaxub", "ab"), 66: ("vmaxuh", "ab"), 130: ("vmaxuw", "ab"),
    258: ("vmaxsb", "ab"), 322: ("vmaxsh", "ab"), 386: ("vmaxsw", "ab"),
    514: ("vminub", "ab"), 578: ("vminuh", "ab"), 642: ("vminuw", "ab"),
    770: ("vminsb", "ab"), 834: ("vminsh", "ab"), 898: ("vminsw", "ab"),
    1026: ("vavgub", "ab"), 1090: ("vavguh", "ab"), 1154: ("vavguw", "ab"),
    1282: ("vavgsb", "ab"), 1346: ("vavgsh", "ab"), 1410: ("vavgsw", "ab"),
    260: ("vslb", "ab"), 324: ("vslh", "ab"), 388: ("vslw", "ab"),
    516: ("vsrb", "ab"), 580: ("vsrh", "ab"), 644: ("vsrw", "ab"),
    772: ("vsrab", "ab"), 836: ("vsrah", "ab"), 900: ("vsraw", "ab"),
    4: ("vrlb", "ab"), 68: ("vrlh", "ab"), 132: ("vrlw", "ab"),
    452: ("vsl", "ab"), 708: ("vsr", "ab"), 1036: ("vslo", "ab"), 1100: ("vsro", "ab"),
    1028: ("vand", "ab"), 1092: ("vandc", "ab"), 1156: ("vor", "ab"), 1220: ("vxor", "ab"),
    1284: ("vnor", "ab"),
    524: ("vspltb", "buimm"), 588: ("vsplth", "buimm"), 652: ("vspltw", "buimm"),
    780: ("vspltisb", "simm"), 844: ("vspltish", "simm"), 908: ("vspltisw", "simm"),
    12: ("vmrghb", "ab"), 76: ("vmrghh", "ab"), 140: ("vmrghw", "ab"),
    268: ("vmrglb", "ab"), 332: ("vmrglh", "ab"), 396: ("vmrglw", "ab"),
    14: ("vpkuhum", "cab"), 78: ("vpkuwum", "cab"), 142: ("vpkuhus", "cab"), 206: ("vpkuwus", "cab"),
    270: ("vpkshus", "cab"), 334: ("vpkswus", "cab"), 398: ("vpkshss", "cab"), 462: ("vpkswss", "cab"),
    526: ("vupkhsb", "b"), 654: ("vupklsb", "b"), 590: ("vupkhsh", "b"), 718: ("vupklsh", "b"),
    520: ("vmuleub", "ab"), 8: ("vmuloub", "ab"), 776: ("vmulesb", "ab"), 264: ("vmulosb", "ab"),
    584: ("vmuleuh", "ab"), 72: ("vmulouh", "ab"), 840: ("vmulesh", "ab"), 328: ("vmulosh", "ab"),
    1928: ("vsumsws", "cab"), 1672: ("vsum2sws", "cab"), 1544: ("vsum4ubs", "cab"),
    1800: ("vsum4sbs", "cab"), 1608: ("vsum4shs", "cab"),
}


def t_vmx(i, fn):
    w = i.w
    d, a, b, c = i.rd, i.ra, i.rb, i.rc_reg
    x6 = w & 0x3F
    if x6 == 44:
        return "vsldoi", f"{v(d)} = vmx_vsldoi({v(a)}, {v(b)}, {(w >> 6) & 15});"
    if x6 in VMX_VA:
        name, order = VMX_VA[x6]
        regs = {"a": v(a), "b": v(b), "c": v(c)}
        return name, f"{v(d)} = vmx_{name}({', '.join(regs[k] for k in order)});"
    if 32 <= x6 <= 47:
        return None
    x10 = w & 0x3FF
    if x10 in VMX_CMP:
        name = VMX_CMP[x10]
        code = f"{v(d)} = vmx_{name}({v(a)}, {v(b)});"
        if (w >> 10) & 1:
            setter = "vmx_set_cr6_bounds" if name == "vcmpbfp" else "vmx_set_cr6"
            code += f" {setter}(ctx, {v(d)});"
            name += "."
        return name, code
    x11 = w & 0x7FF
    if x11 == 1540:
        return "mfvscr", f"{v(d)} = PPCVR{{}}; VW({v(d)}, 3) = ctx.vscr;"
    if x11 == 1604:
        return "mtvscr", f"ctx.vscr = VW({v(b)}, 3);"
    if x11 not in VMX_VX:
        return None
    name, kind = VMX_VX[x11]
    fn_name = f"vmx_{name}"
    args = {"ab": f"{v(a)}, {v(b)}", "b": v(b), "cab": f"ctx, {v(a)}, {v(b)}",
            "buimm": f"{v(b)}, {a}", "cbuimm": f"ctx, {v(b)}, {a}", "simm": str(sext(a, 5))}[kind]
    return name, f"{v(d)} = {fn_name}({args});"


VMX_MEM31 = {  # xo10: (name, kind)
    103: ("lvx", "ld"), 359: ("lvxl", "ld"), 7: ("lvebx", "ld"), 39: ("lvehx", "ld"), 71: ("lvewx", "ld"),
    519: ("lvlx", "lvlx"), 775: ("lvlxl", "lvlx"), 551: ("lvrx", "lvrx"), 807: ("lvrxl", "lvrx"),
    6: ("lvsl", "lvsl"), 38: ("lvsr", "lvsr"),
    231: ("stvx", "st"), 487: ("stvxl", "st"), 135: ("stvebx", "stvebx"), 167: ("stvehx", "stvehx"),
    199: ("stvewx", "stvewx"), 647: ("stvlx", "stvlx"), 903: ("stvlxl", "stvlx"),
    679: ("stvrx", "stvrx"), 935: ("stvrxl", "stvrx"),
}


def t_vmx_mem(i):
    name, kind = VMX_MEM31[i.xo10]
    ea = f"({ra0(i.ra)} + {r(i.rb)})"
    d = i.rd
    code = {
        "ld": f"{v(d)} = vmx_load(base, ea);",
        "lvlx": f"{v(d)} = vmx_lvlx(base, ea);",
        "lvrx": f"{v(d)} = vmx_lvrx(base, ea);",
        "lvsl": f"{v(d)} = vmx_lvsl(ea);",
        "lvsr": f"{v(d)} = vmx_lvsr(ea);",
        "st": f"vmx_store(base, ea, {v(d)});",
        "stvebx": f"vmx_stvebx(base, ea, {v(d)});",
        "stvehx": f"vmx_stvehx(base, ea, {v(d)});",
        "stvewx": f"vmx_stvewx(base, ea, {v(d)});",
        "stvlx": f"vmx_stvlx(base, ea, {v(d)});",
        "stvrx": f"vmx_stvrx(base, ea, {v(d)});",
    }[kind]
    return name, f"{{ uint64_t ea = {ea}; {code} }}"


def translate(word, addr, fn):
    i = Insn(word, addr)
    o = i.opcd
    try:
        if o in (16, 18):
            res = t_branch(i, fn)
        elif o == 19:
            res = t_op19(i, fn)
        elif o in (7, 8, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29):
            res = t_dform(i, fn)
        elif o in (20, 21, 23):
            res = t_rotate32(i, fn)
        elif o == 30:
            res = t_rotate64(i, fn)
        elif o == 31:
            res = t_op31(i, fn)
        elif 32 <= o <= 55 or o in (58, 62):
            res = t_mem(i, fn)
        elif o in (59, 63):
            res = t_fp(i, fn)
        elif o in (2, 3):
            res = t_trap_imm(i, fn)
        elif o == 17:
            res = ("sc", f"PPC_SYSCALL(0x{addr:X});")
        elif o == 4:
            res = t_vmx(i, fn)
        else:
            res = None
    except KeyError:
        res = None
    if res is None:
        return mnemonic_guess(i), None
    return res


def mnemonic_guess(i):
    o = i.opcd
    if o == 4:
        return f"vmx_{i.w & 0x3F:02x}" if (i.w & 0x3F) in range(32, 48) else f"vmx_{i.w & 0x7FF:03x}"
    if o == 31:
        return f"op31_{i.xo10}"
    if o == 19:
        return f"op19_{i.xo10}"
    if o in (59, 63):
        return f"op{o}_{i.xo10 if o == 63 else i.xo5}"
    return f"op{o}"
