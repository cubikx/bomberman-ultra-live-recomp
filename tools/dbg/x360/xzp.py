# usage: xzp.py <file.xzp|xex> <outdir> [base_offset] [filter]: list / extract an XUIZ package
# (XUIResource2.xzp at offset 0; the one embedded in default_dec.xex at 0x503000)
import struct,sys,os
d=open(sys.argv[1],'rb').read()
b=int(sys.argv[3],0) if len(sys.argv)>3 else 0
flt=sys.argv[4].lower() if len(sys.argv)>4 else ''
assert d[b:b+4]==b'XUIZ'
cnt=struct.unpack('>H',d[b+0x14:b+0x16])[0]
p=b+0x16; ents=[]
for i in range(cnt):
    size,off=struct.unpack('>II',d[p:p+8]); n=d[p+8]; p+=9
    name=d[p:p+2*n].decode('utf-16-be'); p+=2*n
    ents.append((name,size,off))
os.makedirs(sys.argv[2],exist_ok=True)
for name,size,off in ents:
    if flt and flt not in name.lower(): continue
    print(name,size)
    out=os.path.join(sys.argv[2],name.replace(chr(92),'/').lstrip('./'))
    os.makedirs(os.path.dirname(out) or '.',exist_ok=True)
    open(out,'wb').write(d[p+off:p+off+size])
