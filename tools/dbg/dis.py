# usage (from the project folder): python -I tools/dbg/dis.py <lo hex> <hi hex> -> disassembly from the generated code with TOC strings resolved
import struct, sys, re, glob
d=open('NPUB30051/USRDIR/EBOOT.elf','rb').read()
segs=[(0x10000,0,0x19c7c8),(0x1b0000,0x1a0000,0x279f0)]
def v2f(v):
    for va,off,fsz in segs:
        if va<=v<va+fsz: return off+v-va
def u32(v):
    f=v2f(v); return struct.unpack('>I',d[f:f+4])[0] if f is not None else None
def cstr(v):
    f=v2f(v)
    if f is None: return None
    e=d.find(b'\0',f); s=d[f:e]
    return s.decode('latin1') if 3<=len(s)<200 and all(32<=c<127 or c in (9,10) for c in s) else None
TOC=0x1bc408
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
lines=[]
for fn in sorted(glob.glob('game/generated/ppu_recomp_*.cpp')):
    for l in open(fn):
        m=re.match(r'\s*// ([0-9A-F]{8}): (.*)',l)
        if m:
            a=int(m.group(1),16)
            if lo<=a<hi: lines.append((a,m.group(2)))
        elif l.startswith('PPU_FUNC(sub_'):
            a=int(l[13:21],16)
            if lo<=a<hi: lines.append((a-0.5,'---- '+l.strip()))
lines.sort()
for a,t in lines:
    note=''
    m=re.match(r'lwz r\d+,(-?\d+)\(r2\)',t)
    if m:
        p=u32(TOC+int(m.group(1)))
        if p is not None:
            s=cstr(p); note=' ; =%#x %s'%(p, repr(s) if s else '')
    print(('%08X '%int(a)) if a==int(a) else '         ', t+note)
