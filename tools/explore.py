"""Recursive-descent code discovery. Prints stats: segments, ints, ports, seg-reg writes, suspicious things."""
import sys, struct, collections
sys.path.insert(0, "tools")
from mz import *
from iced_x86 import *

img = Image(sys.argv[1])
entry = (img.cs + LOADSEG, img.ip)
work = [entry]
seen = {}          # lin -> (seg, off, ins)
ints = collections.Counter()
ports = collections.Counter()
segwrites = collections.Counter()
indirect = []
farptr_targets = set()
mnem = collections.Counter()
cs_writes = []  # writes with CS override

def push(seg, off, why=""):
    lin = img.lin(seg, off)
    if lin not in seen and img.base <= lin < img.end + 0x10000:
        work.append((seg, off))

while work:
    seg, off = work.pop()
    lin = img.lin(seg, off)
    if lin in seen:
        continue
    dec = Decoder(16, bytes(img.mem[lin:lin + 32]), ip=off)
    ins = dec.decode()
    if ins.is_invalid:
        continue
    seen[lin] = (seg, off, ins)
    mnem[ins.mnemonic] += 1
    fc = ins.flow_control
    if ins.mnemonic in (Mnemonic.IN, Mnemonic.OUT, Mnemonic.INSB, Mnemonic.OUTSB):
        if ins.op0_kind == OpKind.IMMEDIATE8 or ins.op1_kind == OpKind.IMMEDIATE8:
            ports[(ins.mnemonic == Mnemonic.OUT, ins.immediate8)] += 1
        else:
            ports[(ins.mnemonic == Mnemonic.OUT, "dx")] += 1
    if ins.mnemonic == Mnemonic.INT:
        ints[ins.immediate8] += 1
    if ins.mnemonic in (Mnemonic.MOV, Mnemonic.POP) and ins.op0_kind == OpKind.REGISTER and ins.op0_register in (Register.DS, Register.ES, Register.SS):
        segwrites[(ins.op0_register, fmt.format(ins))] += 1
    if ins.segment_prefix == Register.CS and ins.op0_kind == OpKind.MEMORY and ins.mnemonic not in (Mnemonic.CMP, Mnemonic.TEST):
        cs_writes.append((seg, off, fmt.format(ins)))
    nxt = (seg, (off + ins.len) & 0xFFFF)
    if fc == FlowControl.NEXT:
        push(*nxt)
    elif fc == FlowControl.UNCONDITIONAL_BRANCH:
        if ins.op0_kind == OpKind.NEAR_BRANCH16:
            push(seg, ins.near_branch16)
        elif ins.op0_kind == OpKind.FAR_BRANCH16:
            push(ins.far_branch_selector, ins.far_branch16)
    elif fc == FlowControl.CONDITIONAL_BRANCH:
        push(*nxt); push(seg, ins.near_branch16)
    elif fc == FlowControl.CALL:
        push(*nxt)
        if ins.op0_kind == OpKind.NEAR_BRANCH16:
            push(seg, ins.near_branch16)
        elif ins.op0_kind == OpKind.FAR_BRANCH16:
            push(ins.far_branch_selector, ins.far_branch16)
    elif fc in (FlowControl.INDIRECT_BRANCH, FlowControl.INDIRECT_CALL):
        indirect.append((seg, off, fmt.format(ins)))
        if fc == FlowControl.INDIRECT_CALL:
            push(*nxt)
    elif fc == FlowControl.INTERRUPT:
        push(*nxt)
    elif fc == FlowControl.RETURN:
        pass
    else:
        push(*nxt)

segs = collections.Counter(s for s, o, i in seen.values())
print("instructions:", len(seen))
print("code segments:", sorted((hex(s), n) for s, n in segs.items()))
print("ints:", sorted((hex(k), v) for k, v in ints.items()))
print("ports:", sorted(((k[0], k[1] if isinstance(k[1], str) else hex(k[1])), v) for k, v in ports.items()))
print("indirect:", len(indirect))
for x in indirect[:80]:
    print("  %04x:%04x %s" % x)
print("segwrites:")
for k, v in sorted(segwrites.items(), key=lambda kv: -kv[1])[:40]:
    print("  ", k[1], v)
print("cs-override mem writes:", len(cs_writes))
for x in cs_writes[:30]:
    print("  %04x:%04x %s" % x)
print("mnemonics:", [(Mnemonic(k).name if hasattr(Mnemonic,'name') else k, v) for k, v in mnem.most_common(60)])
# coverage: linear ranges
lins = sorted(seen)
ranges = []
for l in lins:
    ln = seen[l][2].len
    if ranges and ranges[-1][1] >= l:
        ranges[-1][1] = max(ranges[-1][1], l + ln)
    else:
        ranges.append([l, l + ln])
print("code ranges:", len(ranges), "covered bytes:", sum(b - a for a, b in ranges))
for a, b in ranges:
    if b - a > 64:
        print("  %05x-%05x (%d)" % (a, b, b - a))
