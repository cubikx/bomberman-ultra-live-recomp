"""Decrypted PS3 executable (ELF64 big-endian) loader for the recompiler."""
import struct

PT_LOAD, PT_TLS, PT_PROC_PARAM, PT_PRX_PARAM = 1, 7, 0x60000001, 0x60000002


class Elf:
    def __init__(self, data: bytes):
        self.d = data
        assert data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 2, "need ELF64 big-endian"
        (self.type, self.machine, _, self.entry, phoff, _, _, _, phentsz, phnum,
         _, _, _) = struct.unpack_from(">HHIQQQIHHHHHH", data, 16)
        # (type, flags, offset, vaddr, paddr, filesz, memsz, align)
        self.ph = [struct.unpack_from(">IIQQQQQQ", data, phoff + i * phentsz) for i in range(phnum)]
        self.loads = [p for p in self.ph if p[0] == PT_LOAD and p[6]]
        self.code = [(p[3], p[3] + p[5]) for p in self.loads if p[1] & 1]

    def phdr(self, p_type):
        return next((p for p in self.ph if p[0] == p_type), None)

    def v2o(self, va):
        for _, _, off, vaddr, _, filesz, _, _ in self.loads:
            if vaddr <= va < vaddr + filesz:
                return off + va - vaddr
        return None

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code)

    def u32(self, va):
        return struct.unpack_from(">I", self.d, self.v2o(va))[0]

    def cstr(self, va):
        o = self.v2o(va)
        return self.d[o:self.d.index(b"\0", o)].decode(errors="replace")

    def entry_descriptor(self):
        """_start's (code address, TOC)."""
        return self.u32(self.entry), self.u32(self.entry + 4)

    def imports(self):
        """[(module, nid, slot_addr)] from the sys_proc_prx_param libstub table."""
        prx = self.phdr(PT_PRX_PARAM)
        if not prx:
            return []
        _, magic, _, _, _, _, stub_s, stub_e = struct.unpack_from(">IIIIIIII", self.d, prx[2])
        assert magic == 0x1B434CEC, hex(magic)
        out, va = [], stub_s
        while va < stub_e:
            so = self.v2o(va)
            (esz, _, _, _, nfunc, _, _, _, _, _, name, nids, addrs, _, _) = \
                struct.unpack_from(">BBHHHHHBBHIIIII", self.d, so)
            module = self.cstr(name)
            for i in range(nfunc):
                out.append((module, self.u32(nids + 4 * i), addrs + 4 * i))
            va += esz
        return out
