"""Full listing of discovered code (recursive descent + switch tables) to analysis/listing.asm"""
import sys, struct, collections
sys.path.insert(0, "tools")
from mz import *
from iced_x86 import *

def discover(img, seeds, verbose=False):
    work = list(seeds)
    seen = {}
    indirect = []
    while work:
        seg, off = work.pop()
        lin = img.lin(seg, off)
        if lin in seen or not (img.base <= lin < img.end + 0x10000):
            continue
        dec = Decoder(16, bytes(img.mem[lin:lin + 32]), ip=off)
        ins = dec.decode()
        if ins.is_invalid:
            continue
        seen[lin] = (seg, off, ins)
        fc = ins.flow_control
        nxt = (seg, (off + ins.len) & 0xFFFF)
        if fc == FlowControl.NEXT or fc == FlowControl.INTERRUPT:
            work.append(nxt)
        elif fc == FlowControl.UNCONDITIONAL_BRANCH:
            if ins.op0_kind == OpKind.NEAR_BRANCH16: work.append((seg, ins.near_branch16))
            elif ins.op0_kind == OpKind.FAR_BRANCH16: work.append((ins.far_branch_selector, ins.far_branch16))
        elif fc == FlowControl.CONDITIONAL_BRANCH:
            work.append(nxt); work.append((seg, ins.near_branch16))
        elif fc == FlowControl.CALL:
            work.append(nxt)
            if ins.op0_kind == OpKind.NEAR_BRANCH16: work.append((seg, ins.near_branch16))
            elif ins.op0_kind == OpKind.FAR_BRANCH16: work.append((ins.far_branch_selector, ins.far_branch16))
        elif fc in (FlowControl.INDIRECT_BRANCH, FlowControl.INDIRECT_CALL):
            indirect.append((seg, off, ins))
            if fc == FlowControl.INDIRECT_CALL: work.append(nxt)
            # jmp word [cs:bx+tbl]: table of near offsets right after the jmp typically
            if (ins.op0_kind == OpKind.MEMORY and ins.memory_base == Register.BX and ins.memory_index == Register.NONE
                    and ins.segment_prefix == Register.CS and fc == FlowControl.INDIRECT_BRANCH):
                tbl = ins.memory_displacement & 0xFFFF
                # scan table entries until we reach a byte that's code or invalid
                i = 0
                while i < 256:
                    o = tbl + i * 2
                    if img.lin(seg, o) in seen: break
                    t = img.word(img.lin(seg, o))
                    # sanity: target within the segment near this jmp (same 64K) and decodes validly
                    if t == 0 or abs(t - off) > 0x4000: break
                    work.append((seg, t))
                    i += 1
                if verbose: print("switch at %04x:%04x table %04x entries %d" % (seg, off, tbl, i))
        elif fc == FlowControl.RETURN:
            pass
        else:
            work.append(nxt)
    return seen, indirect

if __name__ == "__main__":
    img = Image(sys.argv[1])
    seeds = [(img.cs + LOADSEG, img.ip)]
    extra = sys.argv[3:] if len(sys.argv) > 3 else []
    for e in extra:
        s, o = e.split(":"); seeds.append((int(s, 16), int(o, 16)))
    seeds += [(0x1D8D, 0x00C8)] + [(0x1D80, o) for o in range(0x2C, 0x44, 2)]
    seen, indirect = discover(img, seeds, verbose=True)
    # second pass: far pointers built as mov ax,OFF ... mov dx,SEG ... push dx; push ax
    for _ in range(3):
        lins = sorted(seen)
        added = 0
        for i, l in enumerate(lins):
            seg, off, ins = seen[l]
            if ins.mnemonic == Mnemonic.PUSH and ins.op0_kind == OpKind.REGISTER and ins.op0_register == Register.AX and i > 0:
                p = seen[lins[i-1]][2]
                if not (p.mnemonic == Mnemonic.PUSH and p.op0_register == Register.DX): continue
                ax = dx = None
                for k in range(2, 9):
                    if i - k < 0: break
                    q = seen[lins[i-k]][2]
                    if q.mnemonic == Mnemonic.MOV and q.op0_kind == OpKind.REGISTER and q.op1_kind == OpKind.IMMEDIATE16:
                        if q.op0_register == Register.AX and ax is None: ax = q.immediate16
                        if q.op0_register == Register.DX and dx is None and (lins[i-k] + 1) in img.reloc_lin: dx = q.immediate16
                    if q.mnemonic == Mnemonic.MOV and q.op0_register == Register.DX and q.op1_register == Register.CS and dx is None: dx = seg
                if ax is not None and dx is not None and img.lin(dx, ax) not in seen and img.base <= img.lin(dx, ax) < img.end:
                    print("seed far ptr %04x:%04x from %04x:%04x" % (dx, ax, seg, off))
                    s2, i2 = discover(img, [(dx, ax)], verbose=True)
                    for k, v in s2.items():
                        if k not in seen: seen[k] = v; added += 1
                    indirect += i2
        if not added: break
    print("instructions", len(seen))
    out = open(sys.argv[2], "w")
    lins = sorted(seen)
    prev_end = img.base
    for l in lins:
        seg, off, ins = seen[l]
        if l > prev_end:
            # data gap
            gap = img.mem[prev_end:l]
            out.write("; ---- data %05x-%05x (%d bytes)\n" % (prev_end, l, l - prev_end))
            for i in range(0, min(len(gap), 512), 32):
                chunk = gap[i:i+32]
                asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
                out.write(";   %05x: %s  %s\n" % (prev_end + i, chunk.hex(), asc))
            if len(gap) > 512: out.write(";   ...\n")
        if l < prev_end:
            out.write("; (overlap)\n")
        b = img.mem[l:l + ins.len].hex()
        rl = "*" if any((l + k) in img.reloc_lin for k in range(ins.len)) else " "
        out.write("%05x %04x:%04x %s%-16s %s\n" % (l, seg, off, rl, b, fmt.format(ins)))
        prev_end = l + ins.len
    out.write("; ---- data %05x-%05x (end)\n" % (prev_end, img.end))
    out.close()
