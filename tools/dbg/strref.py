# usage (from the project folder): python -I tools/dbg/strref.py "string" ...  -> string address, TOC slots pointing at it (grep game/generated for 'lwz rN,<toc offset>(r2)')
import struct, sys, re, glob
d=open('NPUB30051/USRDIR/EBOOT.elf','rb').read()
phoff=struct.unpack('>Q',d[0x20:0x28])[0]; phnum=struct.unpack('>H',d[0x38:0x3a])[0]
segs=[]
for i in range(phnum):
    p=d[phoff+i*56:phoff+i*56+56]
    t,fl,off,va,pa,fsz,msz,al=struct.unpack('>IIQQQQQQ',p)
    if t==1 and fsz: segs.append((va,off,fsz))
def f2v(o):
    for va,off,fsz in segs:
        if off<=o<off+fsz: return va+o-off
def v2f(v):
    for va,off,fsz in segs:
        if va<=v<va+fsz: return off+v-va
toc=struct.unpack('>I',d[v2f(0x1b2104):v2f(0x1b2104)+4])[0]
print('segs',[(hex(a),hex(b)) for a,b,c in segs],'toc',hex(toc))
for s in sys.argv[1:]:
    o=d.find(s.encode()); v=f2v(o)
    # TOC entries holding v
    refs=[]
    for va,off,fsz in segs:
        blob=d[off:off+fsz]
        for m in re.finditer(re.escape(struct.pack('>I',v)),blob):
            if m.start()%4==0: refs.append(va+m.start())
    print(s[:50],'str',hex(v),'ptrs',[hex(r)+'(toc%+d)'%(r-toc) for r in refs])
