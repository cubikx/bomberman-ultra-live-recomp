# usage: x3ref.py <key>... : VA of each UTF-16/ascii string in the 360 image and code refs (lis/addi pairs)
import sys,struct,re
sys.path.insert(0,'D:/xbox 360 games/bomberman live/tools/analysis')
import image
img,_=image.load('D:/xbox 360 games/bomberman live/default_dec.xex')
B=image.BASE
secs=image.sections(img)
code=[(va,sz) for n,va,sz,f in secs if f&0x20000000]
pairs={}
for va,sz in code:
    last={}
    for a in range(va,va+sz,4):
        w=struct.unpack('>I',img[a-B:a-B+4])[0]
        op=w>>26; rd=(w>>21)&31; ra=(w>>16)&31; imm=w&0xffff; simm=imm-0x10000 if imm&0x8000 else imm
        if op==15 and ra==0: last[rd]=(imm<<16,a); continue
        if op in (14,32,34,36,40,48,58) and ra in last and a-last[ra][1]<0x200:
            t=(last[ra][0]+simm)&0xffffffff
            pairs.setdefault(t,[]).append(a)
def funcstart(a):
    # scan back for mflr r12 / or prologue
    for x in range(a,a-0x4000,-4):
        if struct.unpack('>I',img[x-B:x-B+4])[0]==0x7d8802a6: return x
    return None
for k in sys.argv[1:]:
    for enc in ('utf-16-be','ascii'):
        for m in re.finditer(re.escape(k.encode(enc))+(b'\0\0' if enc!='ascii' else b'\0'),img):
            if enc!='ascii' and m.start()%2: continue
            v=B+m.start(); refs=pairs.get(v,[])
            print(k,enc,hex(v),[(hex(r),hex(funcstart(r) or 0)) for r in refs])
