import zipfile,struct,sys
z=zipfile.ZipFile('NPUB30051/USRDIR/media.zip')
names=[n for n in z.namelist() if 'strings_en' in n]
d=z.read(names[0])
# find entries: scan for utf16 strings
i=0
out=[]
# heuristic parse: header then entries: u16 len, utf16 value, u16 len, utf16 key
p=d.find(b'XUIS')
print(names, len(d), d[:16].hex())
def rd(o):
    n=struct.unpack('>H',d[o:o+2])[0]; return d[o+2:o+2+2*n].decode('utf-16-be'), o+2+2*n
o=d.find(b'XUIS')+4
o+=6
cnt=struct.unpack('>H',d[o:o+2])[0]; o+=2
for k in range(cnt):
    v,o=rd(o); key,o=rd(o); out.append((key,v))
for k,v in out: print(k,'=',v)
