"""Cell SPU instruction decoder / disassembler (for analysis; the runtime has its own C++ decoder).

Opcode widths vary (4 to 11 bits, always at the top of the word), so decoding
tries each width from the shortest.
"""

RRR = {0x8: "selb", 0xB: "shufb", 0xC: "mpya", 0xD: "fnms", 0xE: "fma", 0xF: "fms"}
RI18 = {0x21: "ila", 0x08: "hbra", 0x09: "hbrr"}
RI10 = {0x1C: "ai", 0x1D: "ahi", 0x0C: "sfi", 0x0D: "sfhi", 0x14: "andi", 0x15: "andhi", 0x16: "andbi",
        0x04: "ori", 0x05: "orhi", 0x06: "orbi", 0x44: "xori", 0x45: "xorhi", 0x46: "xorbi", 0x74: "mpyi",
        0x75: "mpyui", 0x7C: "ceqi", 0x7D: "ceqhi", 0x7E: "ceqbi", 0x4C: "cgti", 0x4D: "cgthi", 0x4E: "cgtbi",
        0x5C: "clgti", 0x5D: "clgthi", 0x5E: "clgtbi", 0x34: "lqd", 0x24: "stqd", 0x7F: "heqi", 0x4F: "hgti",
        0x5F: "hlgti"}
RI16 = {0x064: "br", 0x060: "bra", 0x066: "brsl", 0x062: "brasl", 0x042: "brnz", 0x040: "brz", 0x046: "brhnz",
        0x044: "brhz", 0x081: "il", 0x083: "ilh", 0x082: "ilhu", 0x0C1: "iohl", 0x065: "fsmbi", 0x061: "lqa",
        0x067: "lqr", 0x041: "stqa", 0x047: "stqr"}
RI8 = {0x1D8: "cflts", 0x1D9: "cfltu", 0x1DA: "csflt", 0x1DB: "cuflt"}
RR = {
    0x1C4: "lqx", 0x144: "stqx", 0x1A8: "bi", 0x1A9: "bisl", 0x1AA: "iret", 0x1AB: "bisled", 0x128: "biz",
    0x129: "binz", 0x12A: "bihz", 0x12B: "bihnz", 0x1AC: "hbr", 0x0C0: "a", 0x0C8: "ah", 0x040: "sf", 0x048: "sfh",
    0x340: "addx", 0x0C2: "cg", 0x342: "cgx", 0x341: "sfx", 0x042: "bg", 0x343: "bgx", 0x3C4: "mpy",
    0x3CC: "mpyu", 0x3C5: "mpyh", 0x3C7: "mpys", 0x3C6: "mpyhh", 0x346: "mpyhha", 0x3CE: "mpyhhu",
    0x34E: "mpyhhau", 0x2A5: "clz", 0x2B4: "cntb", 0x1B6: "fsmb", 0x1B5: "fsmh", 0x1B4: "fsm", 0x1B2: "gbb",
    0x1B1: "gbh", 0x1B0: "gb", 0x0D3: "avgb", 0x053: "absdb", 0x253: "sumb", 0x2B6: "xsbh", 0x2AE: "xshw",
    0x2A6: "xswd", 0x0C1: "and", 0x2C1: "andc", 0x041: "or", 0x2C9: "orc", 0x1F0: "orx", 0x241: "xor",
    0x0C9: "nand", 0x049: "nor", 0x249: "eqv", 0x05F: "shlh", 0x07F: "shlhi", 0x05B: "shl", 0x07B: "shli",
    0x1DB: "shlqbi", 0x1FB: "shlqbii", 0x1DF: "shlqby", 0x1FF: "shlqbyi", 0x1CF: "shlqbybi", 0x05C: "roth",
    0x07C: "rothi", 0x058: "rot", 0x078: "roti", 0x1DC: "rotqby", 0x1FC: "rotqbyi", 0x1CC: "rotqbybi",
    0x1D8: "rotqbi", 0x1F8: "rotqbii", 0x05D: "rothm", 0x07D: "rothmi", 0x059: "rotm", 0x079: "rotmi",
    0x1DD: "rotqmby", 0x1FD: "rotqmbyi", 0x1CD: "rotqmbybi", 0x1D9: "rotqmbi", 0x1F9: "rotqmbii",
    0x05E: "rotmah", 0x07E: "rotmahi", 0x05A: "rotma", 0x07A: "rotmai", 0x3C0: "ceq", 0x3C8: "ceqh",
    0x3D0: "ceqb", 0x240: "cgt", 0x248: "cgth", 0x250: "cgtb", 0x2C0: "clgt", 0x2C8: "clgth", 0x2D0: "clgtb",
    0x3D8: "heq", 0x258: "hgt", 0x2D8: "hlgt", 0x2C4: "fa", 0x2CC: "dfa", 0x2C5: "fs", 0x2CD: "dfs",
    0x2C6: "fm", 0x2CE: "dfm", 0x35C: "dfma", 0x35E: "dfnms", 0x35D: "dfms", 0x35F: "dfnma", 0x1B8: "frest",
    0x1B9: "frsqest", 0x3D4: "fi", 0x3B9: "frds", 0x3B8: "fesd", 0x3C3: "dfceq", 0x3CB: "dfcmeq",
    0x2C3: "dfcgt", 0x2CB: "dfcmgt", 0x3BF: "dftsv", 0x3C2: "fceq", 0x3CA: "fcmeq", 0x2C2: "fcgt",
    0x2CA: "fcmgt", 0x3BA: "fscrwr", 0x398: "fscrrd", 0x00D: "rdch", 0x00F: "rchcnt", 0x10D: "wrch",
    0x000: "stop", 0x140: "stopd", 0x001: "lnop", 0x201: "nop", 0x002: "sync", 0x003: "dsync",
    0x00C: "mfspr", 0x10C: "mtspr", 0x1F4: "cbd", 0x1F5: "chd", 0x1F6: "cwd", 0x1F7: "cdd", 0x1D4: "cbx",
    0x1D5: "chx", 0x1D6: "cwx", 0x1D7: "cdx",
}
# RR-space opcodes that carry a 7-bit immediate instead of rB
RI7 = {"shlhi", "shli", "shlqbii", "shlqbyi", "rothi", "roti", "rotqbyi", "rotqbii", "rothmi", "rotmi",
       "rotqmbyi", "rotqmbii", "rotmahi", "rotmai", "cbd", "chd", "cwd", "cdd", "dftsv"}
BRANCH_RI16 = {"br", "bra", "brsl", "brasl", "brnz", "brz", "brhnz", "brhz"}


def decode(w):
    """-> (mnemonic, form) or (None, None)"""
    op4 = w >> 28
    if op4 in RRR:
        return RRR[op4], "RRR"
    op7 = w >> 25
    if op7 in RI18:
        return RI18[op7], "RI18"
    op8 = w >> 24
    if op8 in RI10:
        return RI10[op8], "RI10"
    op9 = w >> 23
    if op9 in RI16:
        return RI16[op9], "RI16"
    op10 = w >> 22
    if op10 in RI8:
        return RI8[op10], "RI8"
    op11 = w >> 21
    if op11 in RR:
        m = RR[op11]
        return m, "RI7" if m in RI7 else "RR"
    return None, None


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def fields(w):
    return {"rt": w & 0x7F, "ra": (w >> 7) & 0x7F, "rb": (w >> 14) & 0x7F, "rc": (w >> 21) & 0x7F}


def disasm(w, pc):
    m, form = decode(w)
    if m is None:
        return f".long 0x{w:08x}"
    f = fields(w)
    rt, ra, rb, rc = f["rt"], f["ra"], f["rb"], f["rc"]
    if form == "RRR":
        # RRR puts rt in bits 21-27 and rc in 0-6
        return f"{m} ${(w >> 21) & 0x7F},${ra},${rb},${w & 0x7F}"
    if form == "RI18":
        i18 = (w >> 7) & 0x3FFFF
        if m == "ila":
            return f"{m} ${rt},0x{i18:x}"
        return f"{m} 0x{i18 << 2:x}"
    if form == "RI10":
        i10 = sext((w >> 14) & 0x3FF, 10)
        if m in ("lqd", "stqd"):
            return f"{m} ${rt},{i10 * 16}(${ra})"
        return f"{m} ${rt},${ra},{i10}"
    if form == "RI16":
        i16 = sext((w >> 7) & 0xFFFF, 16)
        if m in BRANCH_RI16:
            tgt = (i16 << 2) & 0x3FFFF if m in ("bra", "brasl") else (pc + (i16 << 2)) & 0x3FFFF
            return f"{m} ${rt},0x{tgt:05x}" if m not in ("br", "bra") else f"{m} 0x{tgt:05x}"
        if m in ("lqa", "stqa"):
            return f"{m} ${rt},0x{(i16 << 2) & 0x3FFFF:05x}"
        if m in ("lqr", "stqr"):
            return f"{m} ${rt},0x{(pc + (i16 << 2)) & 0x3FFFF:05x}"
        return f"{m} ${rt},0x{(w >> 7) & 0xFFFF:x}"
    if form == "RI8":
        return f"{m} ${rt},${ra},{(w >> 14) & 0xFF}"
    if form == "RI7":
        return f"{m} ${rt},${ra},{sext((w >> 14) & 0x7F, 7)}"
    if m in ("rdch", "rchcnt"):
        return f"{m} ${rt},ch{ra}"
    if m == "wrch":
        return f"{m} ch{ra},${rt}"
    if m in ("bi", "bisl", "biz", "binz", "bihz", "bihnz", "iret", "bisled"):
        return f"{m} ${rt},${ra}"
    if m in ("stop", "stopd"):
        return f"{m} 0x{w & 0x3FFF:x}"
    return f"{m} ${rt},${ra},${rb}"
