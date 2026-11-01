"""Function discovery and control-flow analysis.

Every function pointer on the PS3 goes through an .opd descriptor, so the
descriptors plus all `bl` targets give the full set of entry points. Each
function's body is the code reachable from its entry; branches into another
function's entry become tail calls, and code shared by several functions is
simply translated once per owner.
"""
from dataclasses import dataclass, field

import ppc


@dataclass
class Function:
    start: int
    insns: dict = field(default_factory=dict)      # pc -> word
    labels: set = field(default_factory=set)       # pcs that need a label
    jump_tables: dict = field(default_factory=dict)  # bctr pc -> [targets]
    calls: set = field(default_factory=set)        # bl targets


@dataclass
class Program:
    elf: object
    toc: int
    entry: int
    functions: dict = field(default_factory=dict)  # start -> Function
    stubs: dict = field(default_factory=dict)      # stub code addr -> (module, nid)
    unresolved_bctr: list = field(default_factory=list)


def _stub_slot(elf, pc):
    """Decode a PS3 import stub at pc and return the descriptor slot it loads, or None."""
    w = [elf.u32(pc + 4 * i) for i in range(8)]
    if (w[0] & 0xFFFF0000) == 0x39800000 and (w[1] & 0xFFFF0000) == 0x658C0000 \
            and (w[2] & 0xFFFF0000) == 0x818C0000 and w[7] == 0x4E800420:
        return ((w[0] & 0xFFFF) | ((w[1] & 0xFFFF) << 16)) + ppc.simm16(w[2])
    return None


def find_stubs(elf):
    slots = {slot: (mod, nid) for mod, nid, slot in elf.imports()}
    stubs = {}
    lo, hi = elf.code[0]
    for pc in range(lo, hi - 28, 4):
        slot = _stub_slot(elf, pc)
        if slot in slots:
            stubs[pc] = slots[slot]
    return stubs


def jump_table(elf, toc, pc):
    """Match the compiler's switch idiom ending in the bctr at pc:

        cmpli crN,0,rI,MAX ; bc 12,4N+1,default
        lwz   rT,d(r2)          # table address from the TOC
        rlwinm/rldic rS,rI,2,.. # index * 4
        lwzx  r0,rS,rT ; extsw r0,r0 ; add r0,r0,rT ; mtctr r0 ; bctr

    Returns the case target list or None.
    """
    window = [(p, elf.u32(p)) for p in range(pc - 4 * 12, pc, 4)]
    base_reg = toc_off = count = lwz_pc = None
    for p, w in reversed(window):
        m = ppc.decode(w)
        if m == "add" and base_reg is None:
            base_reg = ppc.rB(w)
        elif m == "lwz" and base_reg is not None and ppc.rD(w) == base_reg and ppc.rA(w) == 2 \
                and toc_off is None:
            toc_off = ppc.simm16(w)
            lwz_pc = p
        elif m == "cmpli" and toc_off is not None:
            count = ppc.uimm16(w) + 1
            break
    if base_reg is None or toc_off is None:
        return None
    if count is None:
        # bounds check lives in an earlier block: the cmpli whose branch goes to the
        # table code (the nearest cmpli can belong to another test in between)
        for p in range(pc - 4 * 13, max(pc - 4 * 600, 0), -4):
            if ppc.decode(elf.u32(p)) == "cmpli" and ppc.decode(elf.u32(p + 4)) == "bc":
                b = elf.u32(p + 4)
                if ppc.bd(b) + (0 if b & 2 else p + 4) == lwz_pc:
                    count = ppc.uimm16(elf.u32(p)) + 1
                    break
    if count is None:
        # otherwise the nearest one further back
        for p in range(pc - 4 * 13, pc - 4 * 80, -4):
            w = elf.u32(p)
            if ppc.decode(w) == "cmpli":
                count = ppc.uimm16(w) + 1
                break
    base = elf.u32(toc + toc_off)

    def target(i):
        off = elf.u32(base + 4 * i)
        off = off - (1 << 32) if off & 0x80000000 else off
        return (base + off) & 0xFFFFFFFF

    if count is not None and count <= 4096:
        targets = [target(i) for i in range(count)]
        if all(elf.is_code(t) for t in targets):
            return targets
    # no usable bound: read entries while they look like nearby code. Over-reading
    # only adds unreachable labels, so this errs on the safe side.
    targets = []
    for i in range(512):
        t = target(i)
        if not elf.is_code(t) or abs(t - pc) > 0x10000 or t % 4:
            break
        targets.append(t)
    return targets or None


def _walk(prog, f):
    """Collect the instructions reachable from f.start, stopping at other entries."""
    elf, entries = prog.elf, prog.functions
    work = [f.start]
    new_calls = set()
    while work:
        pc = work.pop()
        while True:
            if pc in f.insns:
                break
            if pc != f.start and pc in entries:
                break                                   # falls into another function
            if not elf.is_code(pc):
                f.insns[pc] = None
                break
            w = elf.u32(pc)
            f.insns[pc] = w
            m = ppc.decode(w)
            if m is None:
                break
            if m == "b":
                t = ppc.li(w) + (0 if w & 2 else pc)
                if w & 1:                               # bl
                    if t != pc + 4:
                        f.calls.add(t)
                        new_calls.add(t)
                    pc += 4
                    continue
                if t == f.start or t not in entries:
                    f.labels.add(t)
                    work.append(t)
                break
            if m == "bc":
                t = ppc.bd(w) + (0 if w & 2 else pc)
                if w & 1:                               # bcl: only used as "get PC"
                    pc += 4
                    continue
                if t == f.start or t not in entries:
                    f.labels.add(t)
                    work.append(t)
                if (ppc.BO(w) & 0x14) == 0x14:          # branch always
                    break
                pc += 4
                continue
            if m == "bclr":
                if (ppc.BO(w) & 0x14) == 0x14 and not (w & 1):
                    break
                pc += 4
                continue
            if m == "bcctr":
                if w & 1:                               # bctrl: indirect call
                    pc += 4
                    continue
                targets = jump_table(elf, prog.toc, pc)
                if targets:
                    f.jump_tables[pc] = targets
                    for t in targets:
                        if t == f.start or t not in entries:
                            f.labels.add(t)
                            work.append(t)
                else:
                    prog.unresolved_bctr.append(pc)
                break
            pc += 4
    return new_calls


def analyze(elf):
    entry_pc, toc = elf.entry_descriptor()
    prog = Program(elf=elf, toc=toc, entry=entry_pc)
    prog.stubs = find_stubs(elf)

    # .opd descriptors: (code, toc) pairs in the data segment that share our TOC
    seeds = {entry_pc}
    for _, flags, off, vaddr, _, filesz, _, _ in elf.loads:
        if flags & 1:
            continue
        for va in range(vaddr, vaddr + filesz - 7, 4):
            if elf.u32(va + 4) == toc:
                code = elf.u32(va)
                if elf.is_code(code) and code % 4 == 0:
                    seeds.add(code)
    seeds |= set(prog.stubs)

    for s in seeds:
        prog.functions[s] = Function(s)
    pending = sorted(s for s in seeds if s not in prog.stubs)
    while pending:
        found = set()
        for s in pending:
            found |= _walk(prog, prog.functions[s])
        pending = sorted(t for t in found if t not in prog.functions and elf.is_code(t))
        for t in pending:
            prog.functions[t] = Function(t)
    return prog
