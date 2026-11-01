"""PowerPC 64 (Cell PPU: base + FPU + VMX) instruction decoder.

decode(word) returns the mnemonic, or None for an unknown encoding. Field
helpers use IBM bit numbering (bit 0 = MSB) so they read like the ISA manual.
"""


def bits(w, start, end):
    return (w >> (31 - end)) & ((1 << (end - start + 1)) - 1)


def simm16(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def uimm16(w):
    return w & 0xFFFF


# instruction fields
def rD(w): return bits(w, 6, 10)
def rS(w): return bits(w, 6, 10)
def rA(w): return bits(w, 11, 15)
def rB(w): return bits(w, 16, 20)
def rC(w): return bits(w, 21, 25)
def rc(w): return w & 1
def oe(w): return bits(w, 21, 21)
def crfD(w): return bits(w, 6, 8)
def crfS(w): return bits(w, 11, 13)
def L(w): return bits(w, 10, 10)
def BO(w): return bits(w, 6, 10)
def BI(w): return bits(w, 11, 15)
def SH(w): return bits(w, 16, 20)
def MB(w): return bits(w, 21, 25)
def ME(w): return bits(w, 26, 30)
def SH64(w): return bits(w, 16, 20) | (bits(w, 30, 30) << 5)
def MB64(w): return _mb64(w)
def VD(w): return bits(w, 6, 10)
def VA(w): return bits(w, 11, 15)
def VB(w): return bits(w, 16, 20)
def VC(w): return bits(w, 21, 25)
def SPR(w): return bits(w, 16, 20) << 5 | bits(w, 11, 15)


def _mb64(w):
    # MD-form mb/me field is 6 bits stored as mb[5] || mb[0:4] in bits 21-26
    raw = bits(w, 21, 26)
    return ((raw & 1) << 5) | (raw >> 1)


def bd(w):
    v = w & 0xFFFC
    return v - 0x10000 if v & 0x8000 else v


def li(w):
    v = w & 0x03FFFFFC
    return v - 0x4000000 if v & 0x2000000 else v


def ds(w):
    v = w & 0xFFFC
    return v - 0x10000 if v & 0x8000 else v


OP19 = {0: "mcrf", 16: "bclr", 33: "crnor", 129: "crandc", 150: "isync", 193: "crxor",
        225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}

OP31 = {
    0: "cmp", 4: "tw", 6: "lvsl", 7: "lvebx", 19: "mfcr", 20: "lwarx", 21: "ldx", 23: "lwzx",
    24: "slw", 26: "cntlzw", 27: "sld", 28: "and", 32: "cmpl", 38: "lvsr", 39: "lvehx",
    53: "ldux", 54: "dcbst", 55: "lwzux", 58: "cntlzd", 60: "andc", 68: "td", 71: "lvewx",
    83: "mfmsr", 84: "ldarx", 86: "dcbf", 87: "lbzx", 103: "lvx", 119: "lbzux", 124: "nor",
    135: "stvebx", 144: "mtcrf", 149: "stdx", 150: "stwcx.", 151: "stwx", 167: "stvehx",
    181: "stdux", 183: "stwux", 199: "stvewx", 214: "stdcx.", 215: "stbx", 231: "stvx",
    246: "dcbtst", 247: "stbux", 278: "dcbt", 279: "lhzx", 284: "eqv", 311: "lhzux",
    316: "xor", 339: "mfspr", 341: "lwax", 343: "lhax", 359: "lvxl", 371: "mftb",
    373: "lwaux", 375: "lhaux", 407: "sthx", 412: "orc", 439: "sthux", 444: "or",
    467: "mtspr", 476: "nand", 487: "stvxl", 519: "lvlx", 533: "lswx", 534: "lwbrx",
    535: "lfsx", 536: "srw", 539: "srd", 551: "lvrx", 567: "lfsux", 597: "lswi",
    598: "sync", 599: "lfdx", 631: "lfdux", 647: "stvlx", 661: "stswx", 662: "stwbrx",
    663: "stfsx", 679: "stvrx", 695: "stfsux", 725: "stswi", 727: "stfdx", 759: "stfdux",
    775: "lvlxl", 790: "lhbrx", 792: "sraw", 794: "srad", 807: "lvrxl", 824: "srawi",
    854: "eieio", 903: "stvlxl", 918: "sthbrx", 922: "extsh", 935: "stvrxl", 954: "extsb",
    982: "icbi", 983: "stfiwx", 986: "extsw", 1014: "dcbz",
}
# XO-form (9-bit xo, OE in bit 21)
OP31_XO = {8: "subfc", 9: "mulhdu", 10: "addc", 11: "mulhwu", 40: "subf", 73: "mulhd",
           75: "mulhw", 104: "neg", 136: "subfe", 138: "adde", 200: "subfze", 202: "addze",
           232: "subfme", 233: "mulld", 234: "addme", 235: "mullw", 266: "add",
           457: "divdu", 459: "divwu", 489: "divd", 491: "divw"}

OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls",
        28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
OP63_A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul",
          26: "frsqrte", 28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
OP63_X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1",
          40: "fneg", 64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs",
          264: "fabs", 583: "mffs", 711: "mtfsf", 814: "fctid", 815: "fctidz", 846: "fcfid"}

VA_FORM = {32: "vmhaddshs", 33: "vmhraddshs", 34: "vmladduhm", 36: "vmsumubm", 37: "vmsummbm",
           38: "vmsumuhm", 39: "vmsumuhs", 40: "vmsumshm", 41: "vmsumshs", 42: "vsel",
           43: "vperm", 44: "vsldoi", 46: "vmaddfp", 47: "vnmsubfp"}
VC_FORM = {6: "vcmpequb", 70: "vcmpequh", 134: "vcmpequw", 198: "vcmpeqfp", 454: "vcmpgefp",
           518: "vcmpgtub", 582: "vcmpgtuh", 646: "vcmpgtuw", 710: "vcmpgtfp", 774: "vcmpgtsb",
           838: "vcmpgtsh", 902: "vcmpgtsw", 966: "vcmpbfp"}
VX_FORM = {
    0: "vaddubm", 2: "vmaxub", 4: "vrlb", 8: "vmuloub", 10: "vaddfp", 12: "vmrghb", 14: "vpkuhum",
    64: "vadduhm", 66: "vmaxuh", 68: "vrlh", 72: "vmulouh", 74: "vsubfp", 76: "vmrghh",
    78: "vpkuwum", 128: "vadduwm", 130: "vmaxuw", 132: "vrlw", 140: "vmrghw", 142: "vpkuhus",
    206: "vpkuwus", 258: "vmaxsb", 260: "vslb", 264: "vmulosb", 266: "vrefp", 268: "vmrglb",
    270: "vpkshus", 322: "vmaxsh", 324: "vslh", 328: "vmulosh", 330: "vrsqrtefp", 332: "vmrglh",
    334: "vpkswus", 384: "vaddcuw", 386: "vmaxsw", 388: "vslw", 394: "vexptefp", 396: "vmrglw",
    398: "vpkshss", 452: "vsl", 458: "vlogefp", 462: "vpkswss", 512: "vaddubs", 514: "vminub",
    516: "vsrb", 520: "vmuleub", 522: "vrfin", 524: "vspltb", 526: "vupkhsb", 576: "vadduhs",
    578: "vminuh", 580: "vsrh", 584: "vmuleuh", 586: "vrfiz", 588: "vsplth", 590: "vupkhsh",
    640: "vadduws", 642: "vminuw", 644: "vsrw", 650: "vrfip", 652: "vspltw", 654: "vupklsb",
    708: "vsr", 714: "vrfim", 718: "vupklsh", 768: "vaddsbs", 770: "vminsb", 772: "vsrab",
    776: "vmulesb", 778: "vcfux", 780: "vspltisb", 782: "vpkpx", 832: "vaddshs", 834: "vminsh",
    836: "vsrah", 840: "vmulesh", 842: "vcfsx", 844: "vspltish", 846: "vupkhpx", 896: "vaddsws",
    898: "vminsw", 900: "vsraw", 906: "vctuxs", 908: "vspltisw", 970: "vctsxs", 974: "vupklpx",
    1024: "vsububm", 1026: "vavgub", 1028: "vand", 1034: "vmaxfp", 1036: "vslo", 1088: "vsubuhm",
    1090: "vavguh", 1092: "vandc", 1098: "vminfp", 1100: "vsro", 1152: "vsubuwm", 1154: "vavguw",
    1156: "vor", 1220: "vxor", 1282: "vavgsb", 1284: "vnor", 1346: "vavgsh", 1408: "vsubcuw",
    1410: "vavgsw", 1536: "vsububs", 1540: "mfvscr", 1544: "vsum4ubs", 1600: "vsubuhs",
    1604: "mtvscr", 1608: "vsum4shs", 1664: "vsubuws", 1672: "vsum2sws", 1792: "vsubsbs",
    1800: "vsum4sbs", 1856: "vsubshs", 1920: "vsubsws", 1928: "vsumsws",
}

PRIMARY = {2: "tdi", 3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic",
           13: "addic.", 14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b", 20: "rlwimi",
           21: "rlwinm", 23: "rlwnm", 24: "ori", 25: "oris", 26: "xori", 27: "xoris",
           28: "andi.", 29: "andis.", 32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw",
           37: "stwu", 38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau",
           44: "sth", 45: "sthu", 46: "lmw", 47: "stmw", 48: "lfs", 49: "lfsu", 50: "lfd",
           51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu"}


def decode(w):
    op = w >> 26
    if op in PRIMARY:
        return PRIMARY[op]
    if op == 4:
        if (w & 0x3F) in VA_FORM and (w & 0x3F) >= 32:
            return VA_FORM[w & 0x3F]
        if (w & 0x3FF) in VC_FORM:
            return VC_FORM[w & 0x3FF]
        return VX_FORM.get(w & 0x7FF)
    if op == 19:
        return OP19.get(bits(w, 21, 30))
    if op == 30:
        x3 = bits(w, 27, 29)
        if x3 < 4:
            return ("rldicl", "rldicr", "rldic", "rldimi")[x3]
        x4 = bits(w, 27, 30)
        return {8: "rldcl", 9: "rldcr"}.get(x4)
    if op == 31:
        xo10 = bits(w, 21, 30)
        if xo10 in OP31:
            return OP31[xo10]
        if bits(w, 21, 29) == 413:
            return "sradi"
        return OP31_XO.get(bits(w, 22, 30))
    if op == 58:
        return ("ld", "ldu", "lwa", None)[w & 3]
    if op == 62:
        return ("std", "stdu", None, None)[w & 3]
    if op == 59:
        return OP59.get(bits(w, 26, 30))
    if op == 63:
        a = bits(w, 26, 30)
        if a in OP63_A:
            return OP63_A[a]
        return OP63_X.get(bits(w, 21, 30))
    return None


# ---------------------------------------------------------------------------
# Minimal textual disassembly, for debugging and for comments in generated code.

_D_FORM_LOAD = {"lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu", "lhz", "lhzu",
                "lha", "lhau", "sth", "sthu", "lmw", "stmw"}
_D_FORM_FP = {"lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu"}


_RA_DEST = {"and", "andc", "or", "orc", "xor", "nor", "nand", "eqv", "slw", "srw", "sld", "srd",
            "sraw", "srad", "srawi", "sradi", "extsb", "extsh", "extsw", "cntlzw", "cntlzd"}


def disasm(w, pc):
    m = decode(w)
    if m is None:
        return f".long {w:#010x}"
    if m in _D_FORM_LOAD:
        return f"{m} r{rD(w)},{simm16(w)}(r{rA(w)})"
    if m in _D_FORM_FP:
        return f"{m} f{rD(w)},{simm16(w)}(r{rA(w)})"
    if m in ("ld", "ldu", "lwa", "std", "stdu"):
        return f"{m} r{rD(w)},{ds(w)}(r{rA(w)})"
    if m in ("addi", "addis", "addic", "addic.", "mulli", "subfic"):
        return f"{m} r{rD(w)},r{rA(w)},{simm16(w)}"
    if m in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
        return f"{m} r{rA(w)},r{rS(w)},{uimm16(w):#x}"
    if m in ("cmpi", "cmpli"):
        imm = simm16(w) if m == "cmpi" else uimm16(w)
        return f"{m} cr{crfD(w)},{L(w)},r{rA(w)},{imm}"
    if m in ("cmp", "cmpl"):
        return f"{m} cr{crfD(w)},{L(w)},r{rA(w)},r{rB(w)}"
    if m == "b":
        t = li(w) + (0 if w & 2 else pc)
        return f"b{'l' if w & 1 else ''}{'a' if w & 2 else ''} {t:#x}"
    if m == "bc":
        t = bd(w) + (0 if w & 2 else pc)
        return f"bc{'l' if w & 1 else ''} {BO(w)},{BI(w)},{t:#x}"
    if m in ("bclr", "bcctr"):
        return f"{m}{'l' if w & 1 else ''} {BO(w)},{BI(w)}"
    if m in ("rlwinm", "rlwimi", "rlwnm"):
        return f"{m}{'.' if rc(w) else ''} r{rA(w)},r{rS(w)},{SH(w)},{MB(w)},{ME(w)}"
    if m in ("rldicl", "rldicr", "rldic", "rldimi"):
        return f"{m}{'.' if rc(w) else ''} r{rA(w)},r{rS(w)},{SH64(w)},{MB64(w)}"
    if m in ("mfspr", "mtspr"):
        return f"{m} r{rD(w)},spr{SPR(w)}"
    if m == "sc":
        return "sc"
    op = w >> 26
    if op == 31 and m in _RA_DEST:
        if m == "or" and rS(w) == rB(w):
            return f"mr{'.' if rc(w) else ''} r{rA(w)},r{rS(w)}"
        if m in ("extsb", "extsh", "extsw", "cntlzw", "cntlzd"):
            return f"{m}{'.' if rc(w) else ''} r{rA(w)},r{rS(w)}"
        if m in ("srawi", "sradi"):
            return f"{m}{'.' if rc(w) else ''} r{rA(w)},r{rS(w)},{SH64(w) if m == 'sradi' else SH(w)}"
        return f"{m}{'.' if rc(w) else ''} r{rA(w)},r{rS(w)},r{rB(w)}"
    if op == 31:
        return f"{m}{'.' if rc(w) else ''} r{rD(w)},r{rA(w)},r{rB(w)}"
    if op in (59, 63):
        return f"{m}{'.' if rc(w) else ''} f{rD(w)},f{rA(w)},f{rB(w)},f{rC(w)}"
    if op == 4:
        return f"{m} v{VD(w)},v{VA(w)},v{VB(w)},v{VC(w)}"
    return f"{m} {rD(w)},{rA(w)},{rB(w)}"
