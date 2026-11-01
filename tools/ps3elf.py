#!/usr/bin/env python3
"""Static survey of a decrypted PS3 PPU executable (EBOOT.elf).

Reports segment layout, imported system modules/functions (resolved via NID
hashes of names harvested from the RPCS3 source tree), direct lv2 syscalls,
embedded SPU programs, and a few engine hints.

usage: python -I ps3elf.py <EBOOT.elf> <rpcs3 checkout> [out.json]
"""
import hashlib
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

NID_SUFFIX = bytes.fromhex("6759659904250490566427499489741A")

PT_NAMES = {1: "LOAD", 7: "TLS", 0x60000001: "PROC_PARAM", 0x60000002: "PRX_PARAM",
            0x700000A4: "SCE_PPURELA"}


def nid(name: str) -> int:
    return int.from_bytes(hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:4], "little")


def build_nid_db(rpcs3: Path):
    """Return {(module, nid): name} plus a module-agnostic {nid: name} fallback."""
    by_mod, anymod = {}, {}
    mods = rpcs3 / "rpcs3/Emu/Cell/Modules"
    reg = re.compile(r"REG_(FUNC|VAR|FNID)\(\s*(\w+)\s*,\s*([^,)]+?)\s*(?:,\s*([^)]+?))?\s*\)")
    for src in mods.rglob("*.cpp"):
        for kind, mod, a, b in reg.findall(src.read_text(errors="ignore")):
            if kind == "FNID":
                # REG_FNID(module, "name" | 0xNID, func)
                a = a.strip()
                if a.startswith('"'):
                    n, name = nid(a.strip('"')), a.strip('"')
                else:
                    n, name = int(a, 0), (b or a).strip()
            else:
                name = a.strip()
                n = nid(name)
            by_mod[(mod, n)] = name
            anymod.setdefault(n, name)
    # hard-coded tables in PPUFunction.cpp (sys_libc, sys_libstdcxx, sysPrxForUser ...)
    ppf = rpcs3.parent / "PPUFunction.cpp"
    if ppf.exists():
        cur = []
        for line in ppf.read_text(errors="ignore").splitlines():
            m = re.search(r'_module == "(\w+)"(?: \|\| _module == "(\w+)")?', line)
            if m:
                cur = [x for x in m.groups() if x]
            m = re.search(r'case (0x[0-9a-fA-F]+): return "([^"]+)"', line)
            if m:
                n = int(m.group(1), 16)
                for mod in cur:
                    by_mod.setdefault((mod, n), m.group(2))
                anymod.setdefault(n, m.group(2))
    return by_mod, anymod


class Elf:
    def __init__(self, data: bytes):
        self.d = data
        assert data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 2, "need ELF64 big-endian"
        (self.type, self.machine, _, self.entry, phoff, shoff, _, _, phentsz, phnum,
         shentsz, shnum, shstrndx) = struct.unpack_from(">HHIQQQIHHHHHH", data, 16)
        self.ph = [struct.unpack_from(">IIQQQQQQ", data, phoff + i * phentsz) for i in range(phnum)]
        self.sh = [struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * shentsz) for i in range(shnum)] if shoff else []
        self.shnames = []
        if self.sh and shstrndx < len(self.sh):
            stroff = self.sh[shstrndx][4]
            for s in self.sh:
                end = data.index(b"\0", stroff + s[0])
                self.shnames.append(data[stroff + s[0]:end].decode(errors="replace"))

    def v2o(self, va):
        for p_type, _, off, vaddr, _, filesz, _, _ in self.ph:
            if p_type == 1 and vaddr <= va < vaddr + filesz:
                return off + va - vaddr
        return None

    def u32(self, va):
        return struct.unpack_from(">I", self.d, self.v2o(va))[0]

    def cstr(self, va):
        o = self.v2o(va)
        return self.d[o:self.d.index(b"\0", o)].decode(errors="replace")


def parse_imports(elf: Elf):
    prx = next((p for p in elf.ph if p[0] == 0x60000002), None)
    if not prx:
        return []
    o = prx[2]
    size, magic, ver, _, ent_s, ent_e, stub_s, stub_e = struct.unpack_from(">IIIIIIII", elf.d, o)
    assert magic == 0x1B434CEC, hex(magic)
    out, va = [], stub_s
    while va < stub_e:
        so = elf.v2o(va)
        esz = elf.d[so]
        (_, _, _, attr, nfunc, nvar, ntls, _, _, _, name, nids, addrs, vnids, vstubs) = \
            struct.unpack_from(">BBHHHHHBBHIIIII", elf.d, so)
        out.append({
            "module": elf.cstr(name),
            "funcs": [elf.u32(nids + 4 * i) for i in range(nfunc)],
            "stubs": [elf.u32(addrs + 4 * i) for i in range(nfunc)],
            "vars": [elf.u32(vnids + 4 * i) for i in range(nvar)],
        })
        va += esz
    return out


def scan_code(elf: Elf):
    """Count instruction-level features in executable LOAD segments."""
    stats = Counter()
    syscalls = Counter()
    code_bytes = 0
    for p_type, flags, off, vaddr, _, filesz, _, _ in elf.ph:
        if p_type != 1 or not (flags & 1):
            continue
        code_bytes += filesz
        words = struct.unpack_from(">%dI" % (filesz // 4), elf.d, off)
        last_r11 = None
        for w in words:
            op = w >> 26
            if (w & 0xFFFF0000) == 0x39600000:          # li r11, imm
                last_r11 = w & 0xFFFF
            elif w == 0x44000002:                       # sc
                syscalls[last_r11] += 1
            if w == 0x4E800020:
                stats["blr"] += 1
            elif (w & 0xFFFF0003) == 0xF8210001:        # stdu r1,-x(r1): frame setup
                stats["stdu_r1"] += 1
            if op == 4:
                stats["vmx(op4)"] += 1                  # AltiVec / VMX
            elif op in (59, 63):
                stats["fpu"] += 1
    stats["code_bytes"] = code_bytes
    return stats, syscalls


def find_spu_elfs(data: bytes):
    """Embedded SPU images: big-endian ELF32 with e_machine 0x17."""
    found = []
    for m in re.finditer(rb"\x7fELF\x01\x02\x01", data):
        o = m.start()
        machine = struct.unpack_from(">H", data, o + 18)[0]
        if machine == 0x17:
            phoff, shoff = struct.unpack_from(">II", data, o + 28)
            phnum = struct.unpack_from(">H", data, o + 44)[0]
            size = 0
            for i in range(phnum):
                _, poff, _, _, fsz, _, _, _ = struct.unpack_from(">IIIIIIII", data, o + phoff + 32 * i)
                size = max(size, poff + fsz)
            found.append({"offset": o, "approx_size": size})
    return found


def interesting_strings(data: bytes):
    keys = re.compile(rb"(?i)(spurs|spu_|\.elf|psgl|gcm|havok|bullet|physx|criware|cri_|adx|"
                      rb"bink|fmod|wwise|miles|unreal|gamebryo|renderware|phyre|edge|"
                      rb"\.fpo|\.vpo|\.cg\b|shader|lua|copyright|build|version|compiled)")
    seen = Counter()
    for m in re.finditer(rb"[\x20-\x7e]{5,}", data):
        s = m.group()
        if keys.search(s):
            seen[s.decode()[:120]] += 1
    return seen


def main():
    elf_path, rpcs3 = Path(sys.argv[1]), Path(sys.argv[2])
    out_json = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    data = elf_path.read_bytes()
    elf = Elf(data)
    by_mod, anymod = build_nid_db(rpcs3)

    print(f"# {elf_path.name}: {len(data):,} bytes, type={elf.type:#x} machine={elf.machine:#x} "
          f"entry={elf.entry:#x}")
    print("\n## Program headers")
    for p_type, flags, off, vaddr, _, filesz, memsz, align in elf.ph:
        fl = "".join(c if flags & b else "-" for c, b in (("r", 4), ("w", 2), ("x", 1)))
        print(f"  {PT_NAMES.get(p_type, hex(p_type)):12} {fl} vaddr={vaddr:#010x} "
              f"filesz={filesz:#9x} memsz={memsz:#9x}")
    if elf.shnames:
        print(f"\n## Sections ({len(elf.sh)})")
        for name, s in zip(elf.shnames, elf.sh):
            if name:
                print(f"  {name:24} addr={s[3]:#010x} size={s[5]:#8x}")

    imports = parse_imports(elf)
    report = {"imports": {}, "unresolved": 0}
    total = unresolved = 0
    print(f"\n## Imported modules ({len(imports)})")
    for imp in sorted(imports, key=lambda i: i["module"]):
        names = []
        for n in imp["funcs"]:
            name = by_mod.get((imp["module"], n)) or anymod.get(n)
            if not name:
                name = f"?{n:08x}"
                unresolved += 1
            names.append(name)
        total += len(names)
        report["imports"][imp["module"]] = names
        print(f"\n### {imp['module']} ({len(names)} funcs, {len(imp['vars'])} vars)")
        for name in sorted(names):
            print(f"    {name}")
    report["unresolved"] = unresolved
    print(f"\nTotal imported functions: {total}  (unresolved: {unresolved})")

    stats, syscalls = scan_code(elf)
    print("\n## Code stats")
    for k, v in stats.items():
        print(f"  {k:12} {v:,}")
    print("  direct syscalls (r11 -> count):",
          ", ".join(f"{k}:{v}" for k, v in sorted(syscalls.items(), key=lambda kv: (kv[0] is None, kv[0] or 0))))

    spus = find_spu_elfs(data)
    print(f"\n## Embedded SPU ELF images: {len(spus)}")
    for s in spus:
        print(f"  file offset {s['offset']:#x}, ~{s['approx_size']:,} bytes")

    strs = interesting_strings(data)
    print(f"\n## Interesting strings ({len(strs)})")
    for s, _ in sorted(strs.items()):
        print(f"  {s}")

    if out_json:
        report.update(stats=dict(stats), syscalls={str(k): v for k, v in syscalls.items()},
                      spu_images=spus, strings=sorted(strs))
        out_json.write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
