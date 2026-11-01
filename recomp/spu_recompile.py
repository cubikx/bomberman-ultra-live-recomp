"""Static SPU recompiler: one fixed SPU binary from the EBOOT -> one C++ function.

Each instruction becomes a call to spu::exec<OP>(...) from runtime/src/spu/spu_ops.h
with the instruction word and pc as constants, so the interpreter and the
recompiled code share one definition of every instruction. Direct branches become
gotos; indirect branches go through a switch over every instruction address.
An address outside the binary (e.g. the job's return trap) returns to the
caller, which continues in the interpreter.

usage: python -I recomp/spu_recompile.py <EBOOT.elf> <ea> <size> <ls_base> <entry> <name> <out_dir>
  e.g. python -I recomp/spu_recompile.py NPUB30051/USRDIR/EBOOT.elf 0x1A1F00 42240 0x1000 0x1010 job_1a1f00 game/generated/spu
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf import Elf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LS_MASK = 0x3FFFF

DIRECT = {"BR": "rel", "BRA": "abs", "BRSL": "rel", "BRASL": "abs"}
DIRECT_COND = {"BRZ", "BRNZ", "BRHZ", "BRHNZ"}
INDIRECT = {"BI", "BISL"}
INDIRECT_COND = {"BIZ", "BINZ", "BIHZ", "BIHNZ"}
LINKS = {"BRSL", "BRASL", "BISL"}   # set a return address in rt


def op_table():
    """The interpreter's 2048-entry decode table, rebuilt from spu.cpp's build_table()."""
    src = open(os.path.join(ROOT, "runtime/src/spu/spu.cpp")).read()
    body = src[src.index("build_table()"):]
    body = body[:body.index("return t;")]
    arrays = {m.group(1): re.findall(r"\{(0x[0-9A-Fa-f]+),\s*(\w+)\}", m.group(2))
              for m in re.finditer(r"\}\s*(\w+)\[\]\s*=\s*\{(.*?)\};", body, re.S)}
    table = ["OP_INVALID"] * 2048

    def put(opcode, bits, op):
        shift = 11 - bits
        for i in range(1 << shift):
            table[(opcode << shift) | i] = op

    # direct put(...) calls and "for (auto& e : arr) put(e.op, N, e.e)" loops, in source order
    for m in re.finditer(r"for \(auto& e : (\w+)\) put\(e\.op, (\d+), e\.e\)|put\((0x[0-9A-Fa-f]+), (\d+), (\w+)\)", body):
        if m.group(1):
            for opcode, op in arrays[m.group(1)]:
                put(int(opcode, 16), int(m.group(2)), op)
        else:
            put(int(m.group(3), 16), int(m.group(4)), m.group(5))
    return table


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    elf_path, ea, size, ls_base, entry, name, out_dir = sys.argv[1:8]
    ea, size, ls_base, entry = int(ea, 0), int(size, 0), int(ls_base, 0), int(entry, 0)
    elf = Elf(open(elf_path, "rb").read())
    off = elf.v2o(ea)
    data = elf.d[off:off + size]
    assert len(data) == size, "binary runs past the end of its segment"
    table = op_table()

    insns = []   # (pc, word, op)
    for i in range(0, size - 3, 4):
        w = struct.unpack_from(">I", data, i)[0]
        insns.append((ls_base + i, w, table[w >> 21]))
    end = ls_base + (size & ~3)

    def in_range(a):
        return ls_base <= a < end

    # Every decodable instruction is an entry point: SPU code computes jump targets
    # (relative jump tables, addresses after unconditional branches) that no
    # heuristic finds reliably, and with registers in memory the extra labels cost
    # little. The scan below still documents where the direct targets are.
    labels = {entry} | {pc for pc, _, op in insns if op != "OP_INVALID"}
    for pc, w, op in insns:
        i16 = (w >> 7) & 0xFFFF
        if op in DIRECT or op in DIRECT_COND:
            rel = DIRECT.get(op, "rel") == "rel"
            t = ((pc if rel else 0) + (sext16(i16) << 2)) & LS_MASK
            if in_range(t):
                labels.add(t)
        if op in LINKS and in_range(pc + 4):
            labels.add(pc + 4)
        if in_range(w) and w % 4 == 0:   # possible jump table entry or code pointer
            labels.add(w)
        # code addresses built with immediates (function pointers, computed returns)
        imm = None
        if op == "ILA":
            imm = (w >> 7) & 0x3FFFF
        elif op in ("IL", "IOHL"):
            imm = i16
        if imm is not None and in_range(imm) and imm % 4 == 0:
            labels.add(imm)

    lines = []
    emit = lines.append
    for pc, w, op in insns:
        if pc in labels:
            emit(f"L_{pc:05x}:")
        nxt = (pc + 4) & LS_MASK
        emit(f"\tnext = 0x{nxt:05x};")
        emit(f"\tif (u32 st = exec<{op}>(s, r, 0x{w:08x}u, 0x{pc:05x}, next, code)) return st;")
        i16 = (w >> 7) & 0xFFFF
        if op in DIRECT:
            t = ((pc if DIRECT[op] == "rel" else 0) + (sext16(i16) << 2)) & LS_MASK
            emit(f"\tgoto L_{t:05x};" if in_range(t) else f"\ts.pc = 0x{t:05x}; return 0;")
        elif op in DIRECT_COND:
            t = (pc + (sext16(i16) << 2)) & LS_MASK
            if t != nxt:
                emit(f"\tif (next != 0x{nxt:05x}) " + (f"goto L_{t:05x};" if in_range(t) else "{ s.pc = next; return 0; }"))
        elif op in INDIRECT:
            emit("\ts.pc = next;")
            emit("\tgoto dispatch;")
        elif op in INDIRECT_COND:
            emit(f"\tif (next != 0x{nxt:05x}) {{ s.pc = next; goto dispatch; }}")
    emit(f"\ts.pc = 0x{end & LS_MASK:05x};")
    emit("\treturn 0;")
    emit("dispatch:")
    emit("\tswitch (s.pc & 0x3FFFF) {")
    for a in sorted(labels):
        emit(f"\tcase 0x{a:05x}: goto L_{a:05x};")
    emit("\tdefault: return 0;")
    emit("\t}")

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"spu_{name}.cpp")
    with open(path, "w", newline="\n") as f:
        f.write(f"""// Generated by recomp/spu_recompile.py from {os.path.basename(elf_path)} 0x{ea:08x} ({size} bytes),
// loaded at LS 0x{ls_base:05x}. Do not edit.
#include "spu/compiled.h"
#include "spu/spu_ops.h"

namespace spu {{
namespace {{

u32 run_{name}(SPU& s, u32* code) {{
	Reg* r = s.r;
	u32 next;
	goto dispatch;
""")
        f.write("\n".join(lines))
        f.write(f"""
}}

const CompiledRegistrar registrar({{"{name}", 0x{ea:08x}, {size}, 0x{ls_base:05x}, 0x{fnv1a(data):016x}ull, run_{name}}});

}} // namespace
}} // namespace spu
""")
    ops = {}
    for _, _, op in insns:
        ops[op] = ops.get(op, 0) + 1
    print(f"{path}: {len(insns)} instructions, {len(labels)} entry points, "
          f"{ops.get('OP_INVALID', 0)} undecodable words (data)")


if __name__ == "__main__":
    main()
