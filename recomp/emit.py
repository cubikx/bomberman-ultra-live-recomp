"""PPC instruction -> C++ statement translation.

Generated code talks to the runtime through the names declared in
runtime/include/ppu/recomp.h: the PPUContext fields (ctx.r, ctx.f, ctx.v,
ctx.cr, ...), big-endian memory accessors (ld32/st32/...), and helpers for
the operations that are awkward to write inline.
"""
from ppc import (BI, BO, MB, ME, SH, SH64, MB64, SPR, VA, VB, VC, VD, bits, crfD, crfS, decode,
                 ds, L, oe, rA, rB, rC, rD, rS, rc, simm16, uimm16)

M64 = (1 << 64) - 1


def K(v):
    """u64 literal."""
    return f"0x{v & M64:X}ull"


def mask64(mb, me):
    if mb <= me:
        return ((1 << (64 - mb)) - 1) & ~((1 << (63 - me)) - 1) & M64
    return mask64(mb, 63) | mask64(0, me)      # wraps around


def R(n): return f"ctx.r[{n}]"
def F(n): return f"ctx.f[{n}]"
def V(n): return f"ctx.v[{n}]"
def RA0(n): return "0ull" if n == 0 else R(n)


def cr0(w, reg):
    return f" ppu_cr_s64(ctx, 0, (s64){reg});" if rc(w) else ""


class Unsupported(Exception):
    pass


# --- per-mnemonic translators: (w, pc) -> C++ statement string -------------

def _imm_ea(w):
    return f"(u32)({RA0(rA(w))} + {K(simm16(w))})"


def _ds_ea(w):
    return f"(u32)({RA0(rA(w))} + {K(ds(w))})"


def _x_ea(w):
    return f"(u32)({RA0(rA(w))} + {R(rB(w))})"


LOADS = {"lbz": ("ld8", "u64"), "lhz": ("ld16", "u64"), "lha": ("ld16", "s16"),
         "lwz": ("ld32", "u64"), "lwa": ("ld32", "s32"), "ld": ("ld64", "u64")}
STORES = {"stb": ("st8", "u8"), "sth": ("st16", "u16"), "stw": ("st32", "u32"),
          "std": ("st64", "u64")}


def load(op, kind, ea, d, update_reg=None):
    fn, ty = LOADS[op]
    val = f"{fn}(ea)" if ty == "u64" else f"(u64)(s64)({ty}){fn}(ea)"
    upd = f" {R(update_reg)} = ea;" if update_reg is not None else ""
    return f"{{ u32 ea = {ea}; {R(d)} = {val};{upd} }}"


def store(op, ea, s, update_reg=None):
    fn, ty = STORES[op]
    upd = f" {R(update_reg)} = ea;" if update_reg is not None else ""
    return f"{{ u32 ea = {ea}; {fn}(ea, ({ty}){R(s)});{upd} }}"


def t_mem(m, w):
    # normalise: lwzux -> lwz, stbu -> stb, lhax -> lha
    base = {"lwzu": "lwz", "lwzx": "lwz", "lwzux": "lwz", "lbzu": "lbz", "lbzx": "lbz",
            "lbzux": "lbz", "lhzu": "lhz", "lhzx": "lhz", "lhzux": "lhz", "lhau": "lha",
            "lhax": "lha", "lhaux": "lha", "lwax": "lwa", "lwaux": "lwa", "ldu": "ld",
            "ldx": "ld", "ldux": "ld", "stwu": "stw", "stwx": "stw", "stwux": "stw",
            "stbu": "stb", "stbx": "stb", "stbux": "stb", "sthu": "sth", "sthx": "sth",
            "sthux": "sth", "stdu": "std", "stdx": "std", "stdux": "std"}.get(m, m)
    indexed = m.endswith("x")
    update = m.endswith("u") or m.endswith("ux")
    if base in ("ld", "std", "lwa") and not indexed:
        ea = _ds_ea(w)
    elif indexed:
        ea = _x_ea(w)
    else:
        ea = _imm_ea(w)
    upd = rA(w) if update else None
    if base in LOADS:
        return load(base, None, ea, rD(w), upd)
    return store(base, ea, rS(w), upd)


MEM_OPS = {"lwz", "lwzu", "lwzx", "lwzux", "lbz", "lbzu", "lbzx", "lbzux", "lhz", "lhzu",
           "lhzx", "lhzux", "lha", "lhau", "lhax", "lhaux", "lwa", "lwax", "lwaux", "ld", "ldu",
           "ldx", "ldux", "stw", "stwu", "stwx", "stwux", "stb", "stbu", "stbx", "stbux", "sth",
           "sthu", "sthx", "sthux", "std", "stdu", "stdx", "stdux"}


def t_fmem(m, w):
    indexed = m.endswith("x")
    update = m.endswith("u") or m.endswith("ux")
    base = m.rstrip("x").rstrip("u") if m != "stfiwx" else m
    ea = _x_ea(w) if indexed else _imm_ea(w)
    upd = f" {R(rA(w))} = ea;" if update else ""
    d = rD(w)
    body = {
        "lfs": f"{F(d)} = (double)ldf32(ea);",
        "lfd": f"{F(d)} = ldf64(ea);",
        "stfs": f"stf32(ea, (float){F(d)});",
        "stfd": f"stf64(ea, {F(d)});",
        "stfiwx": f"st32(ea, (u32)std::bit_cast<u64>({F(d)}));",
    }[base]
    return f"{{ u32 ea = {ea}; {body}{upd} }}"


FMEM_OPS = {"lfs", "lfsu", "lfsx", "lfsux", "lfd", "lfdu", "lfdx", "lfdux", "stfs", "stfsu",
            "stfsx", "stfsux", "stfd", "stfdu", "stfdx", "stfdux", "stfiwx"}


def t_int(m, w, pc):
    d, a, b, s = rD(w), rA(w), rB(w), rS(w)
    imm = simm16(w)
    if m in ("add", "subf", "neg", "mullw", "mulhw", "mulhwu", "mulld", "mulhd", "mulhdu",
             "divw", "divwu", "divd", "divdu", "addze", "addc", "adde", "subfc", "subfe",
             "subfze", "addme", "subfme") and oe(w) and m not in ("mulhw", "mulhwu", "mulhd",
                                                                  "mulhdu"):
        raise Unsupported(f"{m}o")
    expr = {
        "add": f"{R(a)} + {R(b)}",
        "subf": f"{R(b)} - {R(a)}",
        "neg": f"0ull - {R(a)}",
        "mullw": f"(u64)((s64)(s32){R(a)} * (s64)(s32){R(b)})",
        "mulhw": f"(u64)(s64)(s32)(((s64)(s32){R(a)} * (s64)(s32){R(b)}) >> 32)",
        "mulhwu": f"(u64)(u32)(((u64)(u32){R(a)} * (u64)(u32){R(b)}) >> 32)",
        "mulld": f"{R(a)} * {R(b)}",
        "mulhd": f"(u64)ppu_mulh_s64({R(a)}, {R(b)})",
        "mulhdu": f"ppu_mulh_u64({R(a)}, {R(b)})",
        "divw": f"ppu_divw({R(a)}, {R(b)})",
        "divwu": f"ppu_divwu({R(a)}, {R(b)})",
        "divd": f"ppu_divd({R(a)}, {R(b)})",
        "divdu": f"ppu_divdu({R(a)}, {R(b)})",
    }.get(m)
    if expr:
        return f"{R(d)} = {expr};" + cr0(w, R(d))
    if m == "addze":
        return (f"{{ u64 x = {R(a)}; u64 r = x + ctx.xer_ca; ctx.xer_ca = r < x; {R(d)} = r; }}"
                + cr0(w, R(d)))
    if m == "addc":
        return (f"{{ u64 x = {R(a)}; u64 r = x + {R(b)}; ctx.xer_ca = r < x; {R(d)} = r; }}"
                + cr0(w, R(d)))
    if m == "adde":
        return (f"{{ u64 x = {R(a)}, y = {R(b)}; u64 r = x + y + ctx.xer_ca; "
                f"ctx.xer_ca = ppu_carry3(x, y, ctx.xer_ca); {R(d)} = r; }}" + cr0(w, R(d)))
    if m == "subfc":
        return (f"{{ u64 x = ~{R(a)}, y = {R(b)}; ctx.xer_ca = ppu_carry3(x, y, 1); "
                f"{R(d)} = x + y + 1; }}" + cr0(w, R(d)))
    if m == "subfe":
        return (f"{{ u64 x = ~{R(a)}, y = {R(b)}; u8 c = ctx.xer_ca; "
                f"ctx.xer_ca = ppu_carry3(x, y, c); {R(d)} = x + y + c; }}" + cr0(w, R(d)))
    if m == "subfze":
        return (f"{{ u64 x = ~{R(a)}; u64 r = x + ctx.xer_ca; ctx.xer_ca = r < x; {R(d)} = r; }}"
                + cr0(w, R(d)))
    if m == "addi":
        return f"{R(d)} = {RA0(a)} + {K(imm)};" if a else f"{R(d)} = {K(imm)};"
    if m == "addis":
        return f"{R(d)} = {RA0(a)} + {K(imm << 16)};" if a else f"{R(d)} = {K(imm << 16)};"
    if m in ("addic", "addic."):
        tail = f" ppu_cr_s64(ctx, 0, (s64){R(d)});" if m == "addic." else ""
        return f"{{ u64 x = {R(a)}; u64 r = x + {K(imm)}; ctx.xer_ca = r < x; {R(d)} = r; }}" + tail
    if m == "subfic":
        return (f"{{ u64 x = {R(a)}; ctx.xer_ca = ppu_carry3(~x, {K(imm)}, 1); "
                f"{R(d)} = {K(imm)} - x; }}")
    if m == "mulli":
        return f"{R(d)} = (u64)((s64){R(a)} * {imm}ll);"

    # logical: rA <- f(rS, ...)
    if m in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
        if m == "ori" and s == 0 and a == 0 and uimm16(w) == 0:
            return "/* nop */"
        u = uimm16(w) << (16 if m.startswith(("oris", "xoris", "andis")) else 0)
        op = {"o": "|", "x": "^", "a": "&"}[m[0]]
        st = f"{R(a)} = {R(s)} {op} {K(u)};"
        if m.startswith("and"):
            st += f" ppu_cr_s64(ctx, 0, (s64){R(a)});"
        return st
    expr = {
        "and": f"{R(s)} & {R(b)}", "andc": f"{R(s)} & ~{R(b)}", "or": f"{R(s)} | {R(b)}",
        "orc": f"{R(s)} | ~{R(b)}", "xor": f"{R(s)} ^ {R(b)}", "nor": f"~({R(s)} | {R(b)})",
        "nand": f"~({R(s)} & {R(b)})", "eqv": f"~({R(s)} ^ {R(b)})",
        "extsb": f"(u64)(s64)(s8){R(s)}", "extsh": f"(u64)(s64)(s16){R(s)}",
        "extsw": f"(u64)(s64)(s32){R(s)}",
        "cntlzw": f"(u64)std::countl_zero((u32){R(s)})",
        "cntlzd": f"(u64)std::countl_zero({R(s)})",
        "slw": f"ppu_slw({R(s)}, {R(b)})", "srw": f"ppu_srw({R(s)}, {R(b)})",
        "sld": f"ppu_sld({R(s)}, {R(b)})", "srd": f"ppu_srd({R(s)}, {R(b)})",
        "sraw": f"ppu_sraw(ctx, {R(s)}, {R(b)})", "srad": f"ppu_srad(ctx, {R(s)}, {R(b)})",
        "srawi": f"ppu_sraw(ctx, {R(s)}, {SH(w)})",
        "sradi": f"ppu_srad(ctx, {R(s)}, {SH64(w)})",
    }.get(m)
    if expr:
        return f"{R(a)} = {expr};" + cr0(w, R(a))

    # rotates
    if m in ("rlwinm", "rlwnm", "rlwimi"):
        msk = K(mask64(MB(w) + 32, ME(w) + 32))
        n = f"(int)({R(b)} & 31)" if m == "rlwnm" else str(SH(w))
        rot = f"std::rotl(ppu_dup32({R(s)}), {n})"
        if m == "rlwimi":
            return f"{R(a)} = ({rot} & {msk}) | ({R(a)} & ~{msk});" + cr0(w, R(a))
        return f"{R(a)} = {rot} & {msk};" + cr0(w, R(a))
    if m in ("rldicl", "rldicr", "rldic", "rldimi"):
        sh, mb = SH64(w), MB64(w)
        msk = {"rldicl": mask64(mb, 63), "rldicr": mask64(0, mb), "rldic": mask64(mb, 63 - sh),
               "rldimi": mask64(mb, 63 - sh)}[m]
        rot = f"std::rotl({R(s)}, {sh})" if sh else R(s)
        if m == "rldimi":
            return f"{R(a)} = ({rot} & {K(msk)}) | ({R(a)} & {K(~msk)});" + cr0(w, R(a))
        return f"{R(a)} = {rot} & {K(msk)};" + cr0(w, R(a))

    # compares
    if m in ("cmp", "cmpl", "cmpi", "cmpli"):
        f = crfD(w)
        signed = m in ("cmp", "cmpi")
        wide = L(w)
        ty = ("s64" if wide else "s32") if signed else ("u64" if wide else "u32")
        rhs = {"cmp": f"({ty}){R(b)}", "cmpl": f"({ty}){R(b)}",
               "cmpi": f"({ty}){imm}", "cmpli": f"({ty}){uimm16(w)}u"}[m]
        fn = "ppu_cmp_s" if signed else "ppu_cmp_u"
        return f"{fn}(ctx, {f}, ({ty}){R(a)}, {rhs});"
    raise Unsupported(m)


CR_LOGIC = {"crand": "a & b", "crandc": "a & !b", "cror": "a | b", "crorc": "a | !b",
            "crxor": "a ^ b", "crnor": "!(a | b)", "crnand": "!(a & b)", "creqv": "!(a ^ b)"}


def t_misc(m, w, pc):
    d, a, b = rD(w), rA(w), rB(w)
    if m in CR_LOGIC:
        e = CR_LOGIC[m].replace("a", f"ctx.cr[{a}]").replace("b", f"ctx.cr[{b}]")
        return f"ctx.cr[{d}] = (u8)({e});"
    if m == "mcrf":
        dd, ss = crfD(w) * 4, crfS(w) * 4
        return " ".join(f"ctx.cr[{dd + i}] = ctx.cr[{ss + i}];" for i in range(4))
    if m == "mfcr":
        return f"{R(d)} = ppu_mfcr(ctx);"
    if m == "mtcrf":
        return f"ppu_mtcrf(ctx, 0x{bits(w, 12, 19):X}, {R(d)});"
    if m in ("mfspr", "mtspr"):
        spr = SPR(w)
        reg = {1: None, 8: "ctx.lr", 9: "ctx.ctr", 256: "ctx.vrsave"}.get(spr, "?")
        if reg == "?":
            raise Unsupported(f"{m} spr{spr}")
        if spr == 1:
            return f"{R(d)} = ppu_get_xer(ctx);" if m == "mfspr" else f"ppu_set_xer(ctx, {R(d)});"
        if m == "mfspr":
            return f"{R(d)} = {reg};"
        return f"{reg} = {'(u32)' if spr == 256 else ''}{R(d)};"
    if m == "mftb":
        return f"{R(d)} = ppu_timebase();"
    if m == "sc":
        return "ppu_syscall(ctx);"
    if m in ("sync", "eieio"):
        return "std::atomic_thread_fence(std::memory_order_seq_cst);"
    if m in ("isync", "dcbt", "dcbtst", "dcbst", "dcbf", "icbi"):
        return f"/* {m} */"
    if m == "dcbz":
        return f"ppu_dcbz({_x_ea(w)});"
    raise Unsupported(m)


def t_fp(m, w, pc):
    d, a, b, c = rD(w), rA(w), rB(w), rC(w)
    if rc(w) and m not in ("fcmpu", "fcmpo"):
        raise Unsupported(f"{m}.")
    single = m.endswith("s") and m not in ("fabs", "fnabs")
    def rnd(e):
        return f"(double)(float)({e})" if single else e
    base = m[:-1] if single else m
    expr = {
        "fadd": f"{F(a)} + {F(b)}", "fsub": f"{F(a)} - {F(b)}", "fmul": f"{F(a)} * {F(c)}",
        "fdiv": f"{F(a)} / {F(b)}", "fsqrt": f"std::sqrt({F(b)})",
        "fmadd": f"std::fma({F(a)}, {F(c)}, {F(b)})",
        "fmsub": f"std::fma({F(a)}, {F(c)}, -{F(b)})",
        "fnmadd": f"-std::fma({F(a)}, {F(c)}, {F(b)})",
        "fnmsub": f"-std::fma({F(a)}, {F(c)}, -{F(b)})",
        "fre": f"1.0 / {F(b)}",
    }.get(base)
    if expr:
        return f"{F(d)} = {rnd(expr)};"
    expr = {
        "fmr": F(b), "fneg": f"-{F(b)}", "fabs": f"std::fabs({F(b)})",
        "fnabs": f"-std::fabs({F(b)})", "frsp": f"(double)(float){F(b)}",
        "frsqrte": f"1.0 / std::sqrt({F(b)})",
        "fsel": f"({F(a)} >= 0.0) ? {F(c)} : {F(b)}",
        "fcfid": f"(double)std::bit_cast<s64>({F(b)})",
        "fctiw": f"std::bit_cast<double>(ppu_fctiw({F(b)}, false))",
        "fctiwz": f"std::bit_cast<double>(ppu_fctiw({F(b)}, true))",
        "fctid": f"std::bit_cast<double>(ppu_fctid({F(b)}, false))",
        "fctidz": f"std::bit_cast<double>(ppu_fctid({F(b)}, true))",
        "mffs": "std::bit_cast<double>((u64)ctx.fpscr)",
    }.get(m)
    if expr:
        return f"{F(d)} = {expr};"
    if m in ("fcmpu", "fcmpo"):
        return f"ppu_fcmp(ctx, {crfD(w)}, {F(a)}, {F(b)});"
    if m == "mtfsf":
        return f"ctx.fpscr = (u32)std::bit_cast<u64>({F(b)});"
    raise Unsupported(m)


def t_vmx(m, w, pc):
    d, a, b, c = VD(w), VA(w), VB(w), VC(w)
    if m in ("lvx", "lvxl"):
        return f"{V(d)} = ldv({_x_ea(w)} & ~15u);"
    if m in ("stvx", "stvxl"):
        return f"stv({_x_ea(w)} & ~15u, {V(d)});"
    if m == "lvsl":
        return f"{V(d)} = ppu_lvsl({_x_ea(w)} & 15u);"
    if m == "lvsr":
        return f"{V(d)} = ppu_lvsr({_x_ea(w)} & 15u);"
    binop = {"vxor": "_mm_xor_si128", "vor": "_mm_or_si128", "vand": "_mm_and_si128",
             "vadduwm": "_mm_add_epi32", "vsubuwm": "_mm_sub_epi32"}.get(m)
    if binop:
        return f"{V(d)}.i = {binop}({V(a)}.i, {V(b)}.i);"
    if m == "vandc":
        return f"{V(d)}.i = _mm_andnot_si128({V(b)}.i, {V(a)}.i);"
    if m == "vnor":
        return f"{V(d)}.i = _mm_xor_si128(_mm_or_si128({V(a)}.i, {V(b)}.i), _mm_set1_epi32(-1));"
    fop = {"vaddfp": "_mm_add_ps", "vsubfp": "_mm_sub_ps", "vmaxfp": "_mm_max_ps",
           "vminfp": "_mm_min_ps"}.get(m)
    if fop:
        return f"{V(d)}.f = {fop}({V(a)}.f, {V(b)}.f);"
    if m == "vmaddfp":
        return f"{V(d)}.f = _mm_add_ps(_mm_mul_ps({V(a)}.f, {V(c)}.f), {V(b)}.f);"
    if m == "vnmsubfp":
        return f"{V(d)}.f = _mm_sub_ps({V(b)}.f, _mm_mul_ps({V(a)}.f, {V(c)}.f));"
    if m == "vsel":
        return (f"{V(d)}.i = _mm_or_si128(_mm_andnot_si128({V(c)}.i, {V(a)}.i), "
                f"_mm_and_si128({V(c)}.i, {V(b)}.i));")
    if m == "vspltw":
        return f"{V(d)}.i = ppu_vspltw({V(b)}, {a & 3});"
    if m == "vspltisw":
        v = a - 32 if a & 16 else a
        return f"{V(d)}.i = _mm_set1_epi32({v});"
    if m == "vsldoi":
        return f"{V(d)} = ppu_vsldoi({V(a)}, {V(b)}, {bits(w, 22, 25)});"
    if m == "vperm":
        return f"{V(d)} = ppu_vperm({V(a)}, {V(b)}, {V(c)});"
    if m == "vmrghw":
        return f"{V(d)} = ppu_vmrghw({V(a)}, {V(b)});"
    if m == "vmrglw":
        return f"{V(d)} = ppu_vmrglw({V(a)}, {V(b)});"
    if m in ("vslw", "vsraw", "vsrw"):
        return f"{V(d)} = ppu_{m}({V(a)}, {V(b)});"
    if m == "vrefp":
        return f"{V(d)}.f = _mm_div_ps(_mm_set1_ps(1.0f), {V(b)}.f);"
    if m == "vrsqrtefp":
        return f"{V(d)}.f = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps({V(b)}.f));"
    if m == "vctsxs":
        return f"{V(d)} = ppu_vctsxs({V(b)}, {a});"
    if m == "vcfsx":
        return f"{V(d)} = ppu_vcfsx({V(b)}, {a});"
    cmp = {"vcmpeqfp": "_mm_castps_si128(_mm_cmpeq_ps({a}.f, {b}.f))",
           "vcmpgtfp": "_mm_castps_si128(_mm_cmpgt_ps({a}.f, {b}.f))",
           "vcmpgefp": "_mm_castps_si128(_mm_cmpge_ps({a}.f, {b}.f))",
           "vcmpequw": "_mm_cmpeq_epi32({a}.i, {b}.i)",
           "vcmpgtsw": "_mm_cmpgt_epi32({a}.i, {b}.i)"}.get(m)
    if cmp:
        st = f"{V(d)}.i = {cmp.format(a=V(a), b=V(b))};"
        if (w >> 10) & 1:
            st += f" ppu_cr6_vec(ctx, {V(d)});"
        return st
    raise Unsupported(m)


def translate(w, pc):
    """Non-branch instruction -> C++ statement. Branches are handled by the function emitter."""
    m = decode(w)
    if m is None:
        raise Unsupported(f".long {w:#010x}")
    if m in MEM_OPS:
        return t_mem(m, w)
    if m in FMEM_OPS:
        return t_fmem(m, w)
    op = w >> 26
    if op in (59, 63):
        return t_fp(m, w, pc)
    if op == 4 or m in ("lvx", "lvxl", "stvx", "stvxl", "lvsl", "lvsr"):
        return t_vmx(m, w, pc)
    if m in CR_LOGIC or m in ("mcrf", "mfcr", "mtcrf", "mfspr", "mtspr", "mftb", "sc", "sync",
                              "eieio", "isync", "dcbt", "dcbtst", "dcbst", "dcbf", "icbi", "dcbz"):
        return t_misc(m, w, pc)
    return t_int(m, w, pc)


def branch_cond(w):
    """(pre-statement, condition) for a conditional branch's BO/BI."""
    bo, bi = BO(w), BI(w)
    pre, parts = "", []
    if not bo & 4:
        pre = "--ctx.ctr; "
        parts.append("ctx.ctr == 0" if bo & 2 else "ctx.ctr != 0")
    if not bo & 16:
        parts.append(f"ctx.cr[{bi}]" if bo & 8 else f"!ctx.cr[{bi}]")
    return pre, " && ".join(parts)
