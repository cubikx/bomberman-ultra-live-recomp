#!/usr/bin/env python3
"""Recompile a decrypted PS3 EBOOT.elf into C++ sources for the runtime.

usage: python -I recompile.py <EBOOT.elf> <out dir> [--rpcs3 <checkout>] [--ppufunc <PPUFunction.cpp>]

The game image itself is NOT embedded in the output: the runtime loads the
segments from the user's own EBOOT.elf at startup.
"""
import argparse
import collections
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import analyze  # noqa: E402
import emit  # noqa: E402
import hooks  # noqa: E402
import nids  # noqa: E402
import ppc  # noqa: E402
from elf import Elf  # noqa: E402

INSNS_PER_FILE = 12000


def fname(addr):
    return f"sub_{addr:08X}"


def emit_function(prog, f, stats):
    funcs = prog.functions
    out = [f"PPU_FUNC({fname(f.start)}) {{", f"\tPPU_ENTER(0x{f.start:X});"]
    pcs = sorted(f.insns)
    if pcs[0] != f.start:
        # the body includes code below the entry point (shared tails, absorbed blocks):
        # code is emitted in address order, so jump to the real entry first
        f.labels.add(f.start)
        out.append(f"\tgoto loc_{f.start:08X};")
        stats["entry_not_first"] += 1
    for i, pc in enumerate(pcs):
        w = f.insns[pc]
        if pc in f.labels:
            out.append(f"loc_{pc:08X}:;")
        if w is None:
            out.append(f"\tppu_trap(ctx, 0x{pc:X}, \"execution left code\"); return;")
            stats["trap"] += 1
            continue
        m = ppc.decode(w)
        comment = f"\t// {pc:08X}: {ppc.disasm(w, pc)}"
        out.append(comment)
        nxt = pcs[i + 1] if i + 1 < len(pcs) else None
        falls = True

        def tail_or_goto(t):
            if t != f.start and t in funcs:
                return f"PPU_TAILCALL({fname(t)});"
            return f"goto loc_{t:08X};"

        if m is None:
            out.append(f"\tppu_trap(ctx, 0x{pc:X}, \"undecodable instruction\"); return;")
            stats["trap"] += 1
            continue
        if m == "b":
            t = ppc.li(w) + (0 if w & 2 else pc)
            if t == pc:
                # "b ." / "bl .": the SDK's deliberate hang after abort()
                out.append(f"\tppu_trap(ctx, 0x{pc:X}, \"guest halted (branch to self, usually abort)\");")
                falls = False
            elif w & 1:
                out.append(f"\tctx.lr = 0x{pc + 4:X};")
                if t == pc + 4:
                    pass
                elif t in funcs and pc in hooks.CALL_HOOKS:
                    out.append(f"\t{hooks.CALL_HOOKS[pc]}(ctx, {fname(t)});")
                    stats["call_hook"] += 1
                elif t in funcs:
                    out.append(f"\t{fname(t)}(ctx);")
                else:
                    out.append(f"\tppu_trap(ctx, 0x{t:X}, \"call to non-code\"); return;")
            else:
                out.append("\t" + tail_or_goto(t))
                falls = False
        elif m == "bc":
            t = ppc.bd(w) + (0 if w & 2 else pc)
            pre, cond = emit.branch_cond(w)
            if w & 1:
                out.append(f"\tctx.lr = 0x{pc + 4:X};")      # bcl: get-PC idiom
            else:
                body = tail_or_goto(t)
                if cond:
                    out.append(f"\t{pre}if ({cond}) {{ {body} }}")
                else:
                    out.append(f"\t{pre}{body}")
                    falls = False
        elif m == "bclr":
            pre, cond = emit.branch_cond(w)
            if w & 1:
                body = f"{{ u64 t = ctx.lr; ctx.lr = 0x{pc + 4:X}; ppu_icall(ctx, (u32)t); }}"
            else:
                body = "return;"
            if cond:
                out.append(f"\t{pre}if ({cond}) {body}")
            else:
                out.append(f"\t{pre}{body}")
                falls = bool(w & 1)
        elif m == "bcctr":
            pre, cond = emit.branch_cond(w)
            if w & 1:
                body = f"{{ ctx.lr = 0x{pc + 4:X}; ppu_icall(ctx, (u32)ctx.ctr); }}"
                out.append(f"\t{pre}if ({cond}) {body}" if cond else f"\t{pre}{body}")
            elif pc in f.jump_tables:
                out.append("\tswitch ((u32)ctx.ctr) {")
                for t in sorted(set(f.jump_tables[pc])):
                    out.append(f"\tcase 0x{t:X}: {tail_or_goto(t)}")
                out.append(f"\tdefault: ppu_trap(ctx, 0x{pc:X}, \"bad switch target\"); return;")
                out.append("\t}")
                falls = False
            else:
                out.append("\tppu_icall(ctx, (u32)ctx.ctr); return;")
                falls = False
        else:
            try:
                out.append("\t" + emit.translate(w, pc))
                stats[m] += 1
            except emit.Unsupported as e:
                out.append(f"\tppu_trap(ctx, 0x{pc:X}, \"unimplemented: {e}\"); return;")
                stats["unsupported:" + str(e)] += 1
        if falls and nxt != pc + 4:
            t = pc + 4
            if t in funcs:
                out.append(f"\tPPU_TAILCALL({fname(t)});")
            else:
                out.append(f"\tppu_trap(ctx, 0x{t:X}, \"fell off function\"); return;")
    out.append("}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--rpcs3", default=str(Path(__file__).parent.parent / "tools/ref/rpcs3"))
    ap.add_argument("--ppufunc", default=str(Path(__file__).parent.parent / "tools/ref/PPUFunction.cpp"))
    args = ap.parse_args()

    data = Path(args.elf).read_bytes()
    elf = Elf(data)
    prog = analyze.analyze(elf)
    by_mod, anymod = nids.load(Path(args.rpcs3), Path(args.ppufunc))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    # imports: one table entry per stub, in address order
    stubs = sorted(prog.stubs.items())
    imports = []
    for i, (addr, (mod, n)) in enumerate(stubs):
        name = by_mod.get((mod, n)) or anymod.get(n) or f"_nid_{n:08X}"
        imports.append((addr, mod, name, n))

    stats = collections.Counter()
    bodies = []
    stub_index = {addr: i for i, (addr, *_rest) in enumerate(imports)}
    for start in sorted(prog.functions):
        f = prog.functions[start]
        if start in stub_index:
            i = stub_index[start]
            # the real stub saves the caller's TOC at 40(r1); callers reload it after the call
            bodies.append((1, f"PPU_FUNC({fname(start)}) {{ st64(u32(ctx.r[1] + 40), ctx.r[2]); "
                              f"ppu_hle_call(ctx, {i}); }} // {imports[i][1]}::{imports[i][2]}"))
        else:
            bodies.append((len(f.insns), emit_function(prog, f, stats)))

    header = ['#pragma once', '#include "ppu/recomp.h"', ""]
    header += [f"PPU_FUNC({fname(a)});" for a in sorted(prog.functions)]
    header += [f"void {h}(PPUContext& ctx, PPUFunc original);" for h in sorted(set(hooks.CALL_HOOKS.values()))]
    (out / "ppu_funcs.h").write_text("\n".join(header) + "\n")

    files, cur, size = [], [], 0
    for n, body in bodies:
        cur.append(body)
        size += n
        if size >= INSNS_PER_FILE:
            files.append(cur)
            cur, size = [], 0
    if cur:
        files.append(cur)
    for i, chunk in enumerate(files):
        text = '#include "ppu_funcs.h"\n\n' + "\n\n".join(chunk) + "\n"
        p = out / f"ppu_recomp_{i:03}.cpp"
        if not p.exists() or p.read_text() != text:       # keep mtimes stable for rebuilds
            p.write_text(text)
    for stale in out.glob("ppu_recomp_*.cpp"):
        if int(stale.stem.split("_")[-1]) >= len(files):
            stale.unlink()

    table = ['#include "ppu_funcs.h"', "", "const PPUFuncEntry g_ppu_funcs[] = {"]
    table += [f"\t{{0x{a:X}, {fname(a)}}}," for a in sorted(prog.functions)]
    table += ["};", f"const size_t g_ppu_func_count = {len(prog.functions)};", ""]
    table += ["const HLEImport g_hle_imports[] = {"]
    table += [f'\t{{0x{a:X}, "{mod}", "{name}", 0x{n:08X}}},' for a, mod, name, n in imports]
    table += ["};", f"const size_t g_hle_import_count = {len(imports)};", ""]
    elf_sha1 = hashlib.sha1(data).hexdigest()
    table += [f'const char g_image_sha1[] = "{elf_sha1}";',
              f"const u32 g_image_entry = 0x{elf.entry:X};",
              f"const u32 g_image_toc = 0x{prog.toc:X};", ""]
    (out / "ppu_tables.cpp").write_text("\n".join(table))

    unsupported = {k: v for k, v in stats.items() if k.startswith("unsupported")}
    print(f"{len(prog.functions)} functions -> {len(files)} source files in {out}")
    print(f"traps: {stats['trap']}, unsupported: {sum(unsupported.values())}, "
          f"functions entered mid-body: {stats['entry_not_first']}, "
          f"call hooks: {stats['call_hook']}/{len(hooks.CALL_HOOKS)}")
    for k, v in sorted(unsupported.items(), key=lambda kv: -kv[1]):
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
