# usage: xuiz.py <xex> <outdir>: extract the XUIZ package embedded at 0x503000
import struct,sys,os
d=open(sys.argv[1],'rb').read()
b=0x503000
print(d[b:b+0x18].hex())
cnt=struct.unpack('>H',d[b+0x14:b+0x16])[0]
p=b+0x16; ents=[]
for i in range(cnt):
    size,off=struct.unpack('>II',d[p:p+8]); n=d[p+8]; p+=9
    name=d[p:p+2*n].decode('utf-16-be'); p+=2*n
    ents.append((name,size,off))
print('cnt',cnt,'table end',hex(p))
for name,size,off in ents:
    if name.endswith('.xur'):
        blob=d[p+off:p+off+size]
        print(name,size,hex(off),blob[:4])
        open(os.path.join(sys.argv[2],name.split(chr(92))[-1]),'wb').write(blob)
