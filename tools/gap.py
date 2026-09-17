"""Show a gap region as hex + tentative disassembly: gap.py START END [seg]"""
import sys; sys.path.insert(0,'tools')
from mz import *
img=Image('original/historik.exe')
a=int(sys.argv[1],16); b=int(sys.argv[2],16)
seg=int(sys.argv[3],16) if len(sys.argv)>3 else a>>4
for i in range(a, min(b, a+256), 32):
    chunk=img.mem[i:min(i+32,b)]
    print("%05x: %s  %s" % (i, chunk.hex(), "".join(chr(c) if 32<=c<127 else "." for c in chunk)))
print("--- tentative disasm")
dec=Decoder(16, bytes(img.mem[a:b]), ip=a-seg*16)
n=0
for ins in dec:
    print("%05x %04x:%04x %s" % (seg*16+ins.ip, seg, ins.ip, fmt.format(ins)))
    n+=1
    if n>60: break
