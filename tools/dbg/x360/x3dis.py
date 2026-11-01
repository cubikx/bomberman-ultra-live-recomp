# usage: x3dis.py <lo> <hi> : 360 disassembly with lis/addi targets resolved to strings
import sys,struct,re
sys.path.insert(0,'D:/xbox 360 games/bomberman live/tools/analysis')
import image,capstone
img,_=image.load('D:/xbox 360 games/bomberman live/default_dec.xex')
B=image.BASE
md=capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
def s(v):
    o=v-B
    if not 0<=o<len(img): return ''
    w=img[o:o+160]
    if len(w)>4 and w[0]==0 and 32<=w[1]<127:
        e=0
        while e+1<len(w) and w[e:e+2]!=b'\0\0': e+=2
        t=w[:e].decode('utf-16-be','replace')
        if len(t)>=2: return 'L"'+t+'"'
    e=w.find(b'\0'); t=w[:e]
    if e>=3 and all(32<=c<127 for c in t): return '"'+t.decode()+'"'
    return ''
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
last={}
for a in range(lo,hi,4):
    insn=next(md.disasm(img[a-B:a-B+4],a),None)
    w=struct.unpack('>I',img[a-B:a-B+4])[0]
    op=w>>26; rd=(w>>21)&31; ra=(w>>16)&31; imm=w&0xffff; simm=imm-0x10000 if imm&0x8000 else imm
    note=''
    if op==15 and ra==0: last[rd]=imm<<16
    elif op in (14,32,34,36,40,48,58) and ra in last:
        t=(last[ra]+simm)&0xffffffff; note=hex(t)+' '+s(t)
    txt=f"{insn.mnemonic} {insn.op_str}" if insn else '.long %08x'%w
    if txt.startswith('mflr r12'): print('----')
    print(f"{a:08X}  {txt:36} {note}")
