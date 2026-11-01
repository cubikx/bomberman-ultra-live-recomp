"""System function names by NID, harvested from an RPCS3 source checkout.

A NID is the first 4 bytes (little-endian) of SHA-1(name + fixed suffix).
"""
import hashlib
import re
from pathlib import Path

NID_SUFFIX = bytes.fromhex("6759659904250490566427499489741A")


def nid(name: str) -> int:
    return int.from_bytes(hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:4], "little")


def load(rpcs3: Path, ppu_function_cpp: Path = None):
    """Return {(module, nid): name} and a module-agnostic {nid: name} fallback."""
    by_mod, anymod = {}, {}
    reg = re.compile(r"REG_(FUNC|VAR|FNID)\(\s*(\w+)\s*,\s*([^,)]+?)\s*(?:,\s*([^)]+?))?\s*\)")
    for src in (rpcs3 / "rpcs3/Emu/Cell/Modules").rglob("*.cpp"):
        for kind, mod, a, b in reg.findall(src.read_text(errors="ignore")):
            a = a.strip()
            if kind == "FNID" and not a.startswith('"'):
                n, name = int(a, 0), (b or a).strip()
            else:
                name = a.strip('"')
                n = nid(name)
            by_mod[(mod, n)] = name
            anymod.setdefault(n, name)
    if ppu_function_cpp and ppu_function_cpp.exists():
        cur = []
        for line in ppu_function_cpp.read_text(errors="ignore").splitlines():
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
