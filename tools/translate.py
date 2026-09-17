"""Static recompiler: translates the discovered 16-bit code of historik.exe
into C. One C function per code segment (cs value); control flow inside a
segment is direct goto, far transfers return the next linear address to the
runtime dispatcher (src/runtime.cpp).

Usage: python3 tools/translate.py original/historik.exe src/gen
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(__file__))
from mz import *
from listing import discover
from iced_x86 import *

REG16 = {Register.AX: "rAX", Register.BX: "rBX", Register.CX: "rCX", Register.DX: "rDX", Register.SI: "rSI", Register.DI: "rDI",
         Register.BP: "rBP", Register.SP: "rSP", Register.CS: "rCS", Register.DS: "rDS", Register.ES: "rES", Register.SS: "rSS"}
REG8 = {Register.AL: "AL", Register.AH: "AH", Register.BL: "BL", Register.BH: "BH", Register.CL: "CL", Register.CH: "CH", Register.DL: "DL", Register.DH: "DH"}
SREG = {Register.CS: "rCS", Register.DS: "rDS", Register.ES: "rES", Register.SS: "rSS"}

MANUAL_SEEDS = [(0x1D8D, 0x00C8)] + [(0x1D80, o) for o in range(0x2C, 0x44, 2)]

class Fn:
    def __init__(self, cs):
        self.cs = cs
        self.ins = {}   # lin -> Instruction

def reg_expr(r):
    if r in REG16: return REG16[r]
    if r in REG8: return REG8[r]
    raise ValueError("reg %r" % r)

def is_reg8(r): return r in REG8

class Emitter:
    def __init__(self, img, fn):
        self.img = img
        self.fn = fn
        self.lines = []
        self.errors = collections.Counter()
        self.seg = fn.cs

    # ---- operand helpers ----
    def ea(self, ins):
        parts = []
        if ins.memory_base != Register.NONE: parts.append(REG16[ins.memory_base])
        if ins.memory_index != Register.NONE: parts.append(REG16[ins.memory_index])
        disp = ins.memory_displacement & 0xFFFF
        if disp or not parts: parts.append("0x%X" % disp)
        return "(uint16_t)(%s)" % " + ".join(parts)

    def addr(self, ins):
        seg = SREG[ins.memory_segment]
        return "LIN(%s, %s)" % (seg, self.ea(ins))

    def opsize(self, ins, i):
        k = ins.op_kind(i)
        if k == OpKind.REGISTER: return 8 if is_reg8(ins.op_register(i)) else 16
        if k == OpKind.MEMORY: return MemorySizeExt.size(ins.memory_size) * 8
        if k == OpKind.IMMEDIATE8: return 8
        return 16

    def imm(self, ins, i):
        k = ins.op_kind(i)
        if k == OpKind.IMMEDIATE8: return "0x%X" % ins.immediate8
        if k == OpKind.IMMEDIATE16: return "0x%X" % ins.immediate16
        if k == OpKind.IMMEDIATE8TO16: return "0x%X" % (ins.immediate8to16 & 0xFFFF)
        if k == OpKind.IMMEDIATE8_2ND: return "0x%X" % ins.immediate8_2nd
        raise ValueError("imm kind %r" % k)

    def read(self, ins, i, size, a=None):
        """C expression reading operand i; `a` is a precomputed address variable for memory operands."""
        k = ins.op_kind(i)
        if k == OpKind.REGISTER: return reg_expr(ins.op_register(i))
        if k == OpKind.MEMORY: return "rd%d(%s)" % (size, a or self.addr(ins))
        return self.imm(ins, i)

    def write(self, ins, i, size, value, a=None):
        k = ins.op_kind(i)
        if k == OpKind.REGISTER: return "%s = %s;" % (reg_expr(ins.op_register(i)), value)
        if k == OpKind.MEMORY: return "wr%d(%s, %s);" % (size, a or self.addr(ins), value)
        raise ValueError("write kind %r" % k)

    def is_mem(self, ins, i): return ins.op_kind(i) == OpKind.MEMORY

    # ---- emission ----
    def emit(self, s): self.lines.append("\t" + s)

    def label(self, lin): return "L_%05x" % lin

    def local(self, lin): return lin in self.fn.ins

    def goto(self, lin, backward):
        """Jump to a linear address inside the segment."""
        pre = "irq_check(); " if backward else ""
        if self.local(lin): return "{ %sgoto %s; }" % (pre, self.label(lin))
        return "{ %sreturn 0x%X; }" % (pre, lin)

    def near_dispatch(self, expr):
        return "NEAR_DISPATCH(%s);" % expr

    def cond(self, m):
        return {
            Mnemonic.JE: "fZF", Mnemonic.JNE: "!fZF", Mnemonic.JB: "fCF", Mnemonic.JAE: "!fCF",
            Mnemonic.JBE: "(fCF | fZF)", Mnemonic.JA: "!(fCF | fZF)", Mnemonic.JL: "(fSF != fOF)", Mnemonic.JGE: "(fSF == fOF)",
            Mnemonic.JLE: "(fZF | (fSF != fOF))", Mnemonic.JG: "(!fZF && fSF == fOF)", Mnemonic.JS: "fSF", Mnemonic.JNS: "!fSF",
            Mnemonic.JO: "fOF", Mnemonic.JNO: "!fOF", Mnemonic.JP: "fPF", Mnemonic.JNP: "!fPF",
        }[m]

    def translate(self, lin, ins):
        m = ins.mnemonic
        seg = self.seg
        off = ins.ip & 0xFFFF
        nxt_ip = (off + ins.len) & 0xFFFF
        E = self.emit
        E("%s: TRACE(0x%05X);" % (self.label(lin), lin))
        E("/* %04x:%04x  %s */" % (seg, off, fmt.format(ins)))

        # ---- special cases ----
        if seg == 0x117D and off == 0:
            E("ctvoice_call(); { uint16_t ip = pop16(); rCS = pop16(); return LIN(rCS, ip); }")
            return

        two_op_alu = {Mnemonic.ADD: "add", Mnemonic.ADC: "adc", Mnemonic.SUB: "sub", Mnemonic.SBB: "sbb",
                      Mnemonic.AND: "and", Mnemonic.OR: "or", Mnemonic.XOR: "xor"}
        if m in two_op_alu:
            size = self.opsize(ins, 0)
            if self.is_mem(ins, 0):
                E("{ uint32_t a = %s; %s }" % (self.addr(ins), self.write(ins, 0, size, "%s%d(%s, %s)" % (two_op_alu[m], size, self.read(ins, 0, size, "a"), self.read(ins, 1, size)), "a")))
            else:
                E(self.write(ins, 0, size, "%s%d(%s, %s)" % (two_op_alu[m], size, self.read(ins, 0, size), self.read(ins, 1, size))))
            return
        if m == Mnemonic.CMP:
            size = self.opsize(ins, 0)
            E("(void)sub%d(%s, %s);" % (size, self.read(ins, 0, size), self.read(ins, 1, size)))
            return
        if m == Mnemonic.TEST:
            size = self.opsize(ins, 0)
            E("(void)and%d(%s, %s);" % (size, self.read(ins, 0, size), self.read(ins, 1, size)))
            return
        if m == Mnemonic.MOV:
            size = self.opsize(ins, 0)
            if ins.op0_kind == OpKind.REGISTER and ins.op0_register in SREG:
                E("%s = %s;" % (SREG[ins.op0_register], self.read(ins, 1, 16)))
                if ins.op0_register == Register.SS: pass
                return
            if ins.op1_kind == OpKind.REGISTER and ins.op1_register in SREG:
                E(self.write(ins, 0, 16, SREG[ins.op1_register]))
                return
            E(self.write(ins, 0, size, self.read(ins, 1, size)))
            return
        if m == Mnemonic.XCHG:
            size = self.opsize(ins, 0)
            if self.is_mem(ins, 0):
                E("{ uint32_t a = %s; uint%d_t t = rd%d(a); wr%d(a, %s); %s = t; }" % (self.addr(ins), size, size, size, self.read(ins, 1, size), self.read(ins, 1, size)))
            elif self.is_mem(ins, 1):
                E("{ uint32_t a = %s; uint%d_t t = rd%d(a); wr%d(a, %s); %s = t; }" % (self.addr(ins), size, size, size, self.read(ins, 0, size), self.read(ins, 0, size)))
            else:
                E("{ uint%d_t t = %s; %s = %s; %s = t; }" % (size, self.read(ins, 0, size), self.read(ins, 0, size), self.read(ins, 1, size), self.read(ins, 1, size)))
            return
        one_op = {Mnemonic.INC: "inc", Mnemonic.DEC: "dec", Mnemonic.NEG: "neg"}
        if m in one_op:
            size = self.opsize(ins, 0)
            if self.is_mem(ins, 0):
                E("{ uint32_t a = %s; wr%d(a, %s%d(rd%d(a))); }" % (self.addr(ins), size, one_op[m], size, size))
            else:
                r = self.read(ins, 0, size); E("%s = %s%d(%s);" % (r, one_op[m], size, r))
            return
        if m == Mnemonic.NOT:
            size = self.opsize(ins, 0)
            if self.is_mem(ins, 0): E("{ uint32_t a = %s; wr%d(a, (uint%d_t)~rd%d(a)); }" % (self.addr(ins), size, size, size))
            else: r = self.read(ins, 0, size); E("%s = (uint%d_t)~%s;" % (r, size, r))
            return
        shifts = {Mnemonic.SHL: "shl", Mnemonic.SHR: "shr", Mnemonic.SAR: "sar", Mnemonic.ROL: "rol", Mnemonic.ROR: "ror", Mnemonic.RCL: "rcl", Mnemonic.RCR: "rcr"}
        if m in shifts:
            size = self.opsize(ins, 0)
            cnt = "CL" if ins.op1_kind == OpKind.REGISTER else self.imm(ins, 1)
            if self.is_mem(ins, 0):
                E("{ uint32_t a = %s; wr%d(a, %s%d(rd%d(a), %s)); }" % (self.addr(ins), size, shifts[m], size, size, cnt))
            else:
                r = self.read(ins, 0, size); E("%s = %s%d(%s, %s);" % (r, shifts[m], size, r, cnt))
            return
        muldiv = {Mnemonic.MUL: "mul", Mnemonic.IMUL: "imul", Mnemonic.DIV: "div", Mnemonic.IDIV: "idiv"}
        if m in muldiv and ins.op_count == 1:
            size = self.opsize(ins, 0)
            E("%s%d(%s);" % (muldiv[m], size, self.read(ins, 0, size)))
            return
        if m == Mnemonic.PUSH:
            if ins.op0_kind == OpKind.REGISTER and ins.op0_register in SREG: E("push16(%s);" % SREG[ins.op0_register])
            elif ins.op0_kind == OpKind.REGISTER and ins.op0_register == Register.SP: E("push16(rSP);")
            else: E("push16(%s);" % self.read(ins, 0, 16))
            return
        if m == Mnemonic.POP:
            if ins.op0_kind == OpKind.REGISTER and ins.op0_register in SREG: E("%s = pop16();" % SREG[ins.op0_register])
            elif self.is_mem(ins, 0): E("{ uint16_t v = pop16(); wr16(%s, v); }" % self.addr(ins))
            else: E("%s = pop16();" % self.read(ins, 0, 16))
            return
        if m == Mnemonic.PUSHF: E("push16(get_flags());"); return
        if m == Mnemonic.POPF: E("set_flags(pop16()); if (fIF) irq_check();"); return
        if m == Mnemonic.PUSHA: E("{ uint16_t sp = rSP; push16(rAX); push16(rCX); push16(rDX); push16(rBX); push16(sp); push16(rBP); push16(rSI); push16(rDI); }"); return
        if m == Mnemonic.POPA: E("rDI = pop16(); rSI = pop16(); rBP = pop16(); rSP = (uint16_t)(rSP + 2); rBX = pop16(); rDX = pop16(); rCX = pop16(); rAX = pop16();"); return
        if m == Mnemonic.LEA: E("%s = %s;" % (reg_expr(ins.op0_register), self.ea(ins))); return
        if m in (Mnemonic.LES, Mnemonic.LDS):
            E("{ uint32_t a = %s; %s = rd16(a); %s = rd16(a + 2); }" % (self.addr(ins), reg_expr(ins.op0_register), "rES" if m == Mnemonic.LES else "rDS"))
            return
        if m == Mnemonic.CBW: E("rAX = (uint16_t)(int16_t)(int8_t)AL;"); return
        if m == Mnemonic.CWD: E("rDX = (rAX & 0x8000) ? 0xFFFF : 0;"); return
        if m == Mnemonic.XLATB: E("AL = rd8(LIN(%s, (uint16_t)(rBX + AL)));" % SREG[ins.memory_segment]); return
        if m == Mnemonic.NOP: return
        if m == Mnemonic.CLC: E("fCF = 0;"); return
        if m == Mnemonic.STC: E("fCF = 1;"); return
        if m == Mnemonic.CMC: E("fCF ^= 1;"); return
        if m == Mnemonic.CLD: E("fDF = 0;"); return
        if m == Mnemonic.STD: E("fDF = 1;"); return
        if m == Mnemonic.CLI: E("fIF = 0;"); return
        if m == Mnemonic.STI: E("fIF = 1; irq_check();"); return
        if m == Mnemonic.HLT: E("irq_check();"); return
        if m == Mnemonic.LAHF: E("AH = (uint8_t)get_flags();"); return
        if m == Mnemonic.SAHF: E("{ uint16_t f = (get_flags() & 0xFF00) | AH; set_flags(f); }"); return
        if m == Mnemonic.IN:
            port = "rDX" if ins.op1_kind == OpKind.REGISTER else self.imm(ins, 1)
            if self.opsize(ins, 0) == 8: E("AL = port_in8(%s);" % port)
            else: E("rAX = (uint16_t)(port_in8(%s) | (port_in8((uint16_t)(%s + 1)) << 8));" % (port, port))
            return
        if m == Mnemonic.OUT:
            port = "rDX" if ins.op0_kind == OpKind.REGISTER else self.imm(ins, 0)
            if self.opsize(ins, 1) == 8: E("port_out8(%s, AL);" % port)
            else: E("port_out8(%s, AL); port_out8((uint16_t)(%s + 1), AH);" % (port, port))
            return
        # ---- string instructions ----
        strops = {Mnemonic.MOVSB: 8, Mnemonic.MOVSW: 16, Mnemonic.STOSB: 8, Mnemonic.STOSW: 16, Mnemonic.LODSB: 8, Mnemonic.LODSW: 16,
                  Mnemonic.SCASB: 8, Mnemonic.SCASW: 16, Mnemonic.CMPSB: 8, Mnemonic.CMPSW: 16}
        if m in strops:
            size = strops[m]; n = size // 8
            sseg = SREG[ins.memory_segment] if m not in (Mnemonic.STOSB, Mnemonic.STOSW, Mnemonic.SCASB, Mnemonic.SCASW) else None
            acc = "AL" if size == 8 else "rAX"
            body = {
                Mnemonic.MOVSB: "wr%d(LIN(rES, rDI), rd%d(LIN(%s, rSI))); rSI += SDIR(%d); rDI += SDIR(%d);" % (size, size, sseg, n, n),
                Mnemonic.MOVSW: "wr%d(LIN(rES, rDI), rd%d(LIN(%s, rSI))); rSI += SDIR(%d); rDI += SDIR(%d);" % (size, size, sseg, n, n),
                Mnemonic.STOSB: "wr%d(LIN(rES, rDI), %s); rDI += SDIR(%d);" % (size, acc, n),
                Mnemonic.STOSW: "wr%d(LIN(rES, rDI), %s); rDI += SDIR(%d);" % (size, acc, n),
                Mnemonic.LODSB: "%s = rd%d(LIN(%s, rSI)); rSI += SDIR(%d);" % (acc, size, sseg, n),
                Mnemonic.LODSW: "%s = rd%d(LIN(%s, rSI)); rSI += SDIR(%d);" % (acc, size, sseg, n),
                Mnemonic.SCASB: "(void)sub%d(%s, rd%d(LIN(rES, rDI))); rDI += SDIR(%d);" % (size, acc, size, n),
                Mnemonic.SCASW: "(void)sub%d(%s, rd%d(LIN(rES, rDI))); rDI += SDIR(%d);" % (size, acc, size, n),
                Mnemonic.CMPSB: "(void)sub%d(rd%d(LIN(%s, rSI)), rd%d(LIN(rES, rDI))); rSI += SDIR(%d); rDI += SDIR(%d);" % (size, size, sseg, size, n, n),
                Mnemonic.CMPSW: "(void)sub%d(rd%d(LIN(%s, rSI)), rd%d(LIN(rES, rDI))); rSI += SDIR(%d); rDI += SDIR(%d);" % (size, size, sseg, size, n, n),
            }[m]
            if ins.has_rep_prefix or ins.has_repe_prefix or ins.has_repne_prefix:
                if m in (Mnemonic.SCASB, Mnemonic.SCASW, Mnemonic.CMPSB, Mnemonic.CMPSW):
                    stop = "if (%s) break;" % ("!fZF" if ins.has_repe_prefix else "fZF")
                    E("while (rCX) { %s rCX--; %s }" % (body, stop))
                else:
                    E("while (rCX) { %s rCX--; }" % body)
            else:
                E(body)
            return
        # ---- control flow ----
        if m == Mnemonic.JMP:
            if ins.op0_kind == OpKind.NEAR_BRANCH16:
                t = self.img.lin(seg, ins.near_branch16); E(self.goto(t, ins.near_branch16 <= off)); return
            if ins.op0_kind == OpKind.FAR_BRANCH16:
                E("rCS = 0x%X; return 0x%X;" % (ins.far_branch_selector, self.img.lin(ins.far_branch_selector, ins.far_branch16))); return
            if ins.op0_kind == OpKind.REGISTER:
                E(self.near_dispatch("LIN(rCS, %s)" % reg_expr(ins.op0_register))); return
            if MemorySizeExt.size(ins.memory_size) == 4:
                E("{ uint32_t a = %s; uint16_t ip = rd16(a); rCS = rd16(a + 2); return LIN(rCS, ip); }" % self.addr(ins)); return
            E(self.near_dispatch("LIN(rCS, rd16(%s))" % self.addr(ins))); return
        if m in (Mnemonic.JE, Mnemonic.JNE, Mnemonic.JB, Mnemonic.JAE, Mnemonic.JBE, Mnemonic.JA, Mnemonic.JL, Mnemonic.JGE, Mnemonic.JLE, Mnemonic.JG,
                 Mnemonic.JS, Mnemonic.JNS, Mnemonic.JO, Mnemonic.JNO, Mnemonic.JP, Mnemonic.JNP):
            t = self.img.lin(seg, ins.near_branch16)
            E("if (%s) %s" % (self.cond(m), self.goto(t, ins.near_branch16 <= off))); return
        if m == Mnemonic.JCXZ:
            t = self.img.lin(seg, ins.near_branch16); E("if (!rCX) %s" % self.goto(t, ins.near_branch16 <= off)); return
        if m in (Mnemonic.LOOP, Mnemonic.LOOPE, Mnemonic.LOOPNE):
            t = self.img.lin(seg, ins.near_branch16)
            c = {Mnemonic.LOOP: "--rCX", Mnemonic.LOOPE: "--rCX && fZF", Mnemonic.LOOPNE: "--rCX && !fZF"}[m]
            E("if (%s) %s" % (c, self.goto(t, True))); return
        if m == Mnemonic.CALL:
            if ins.op0_kind == OpKind.NEAR_BRANCH16:
                t = self.img.lin(seg, ins.near_branch16)
                E("push16(0x%X); %s" % (nxt_ip, self.goto(t, True))); return
            if ins.op0_kind == OpKind.FAR_BRANCH16:
                E("push16(rCS); push16(0x%X); rCS = 0x%X; return 0x%X;" % (nxt_ip, ins.far_branch_selector, self.img.lin(ins.far_branch_selector, ins.far_branch16))); return
            if ins.op0_kind == OpKind.REGISTER:
                E("push16(0x%X); %s" % (nxt_ip, self.near_dispatch("LIN(rCS, %s)" % reg_expr(ins.op0_register)))); return
            if MemorySizeExt.size(ins.memory_size) == 4:
                E("{ uint32_t a = %s; uint16_t ip = rd16(a), cs = rd16(a + 2); push16(rCS); push16(0x%X); rCS = cs; return LIN(cs, ip); }" % (self.addr(ins), nxt_ip)); return
            E("{ uint16_t ip = rd16(%s); push16(0x%X); %s }" % (self.addr(ins), nxt_ip, self.near_dispatch("LIN(rCS, ip)"))); return
        if m == Mnemonic.RET:
            extra = ins.immediate16 if ins.op_count else 0
            E("{ uint16_t ip = pop16(); rSP = (uint16_t)(rSP + %d); %s }" % (extra, self.near_dispatch("LIN(rCS, ip)"))); return
        if m == Mnemonic.RETF:
            extra = ins.immediate16 if ins.op_count else 0
            E("{ uint16_t ip = pop16(); rCS = pop16(); rSP = (uint16_t)(rSP + %d); return LIN(rCS, ip); }" % extra); return
        if m == Mnemonic.IRET:
            E("{ uint16_t ip = pop16(); rCS = pop16(); set_flags(pop16()); return LIN(rCS, ip); }"); return
        if m == Mnemonic.INT:
            E("{ uint32_t t = do_int(0x%X, rCS, 0x%X); if (t) return t; }" % (ins.immediate8, nxt_ip)); return
        if m == Mnemonic.INTO: return
        # unknown
        self.errors[fmt.format(ins)] += 1
        E('fatal("unimplemented at %04x:%04x: %s");' % (seg, off, fmt.format(ins).replace('"', "'")))

def write_if_changed(path, text):
    """Keep the file's timestamp when the content is identical, so an unchanged
    translation does not force a full rebuild on the (slow) Haiku box."""
    try:
        if open(path).read() == text: return False
    except IOError:
        pass
    open(path, "w").write(text)
    return True


def build(img, out_dir):
    seeds = [(img.cs + LOADSEG, img.ip)] + MANUAL_SEEDS
    # reuse listing.py discovery incl. far pointer heuristics by importing its logic
    import listing as L
    seen, indirect = L.discover(img, seeds)
    # far pointer pass (same as listing.py)
    for _ in range(3):
        lins = sorted(seen); added = 0
        for i, l in enumerate(lins):
            seg, off, ins = seen[l]
            if ins.mnemonic == Mnemonic.PUSH and ins.op0_kind == OpKind.REGISTER and ins.op0_register == Register.AX and i > 0:
                p = seen[lins[i - 1]][2]
                if not (p.mnemonic == Mnemonic.PUSH and p.op0_register == Register.DX): continue
                ax = dx = None
                for k in range(2, 9):
                    if i - k < 0: break
                    q = seen[lins[i - k]][2]
                    if q.mnemonic == Mnemonic.MOV and q.op0_kind == OpKind.REGISTER and q.op1_kind == OpKind.IMMEDIATE16:
                        if q.op0_register == Register.AX and ax is None: ax = q.immediate16
                        if q.op0_register == Register.DX and dx is None and (lins[i - k] + 1) in img.reloc_lin: dx = q.immediate16
                    if q.mnemonic == Mnemonic.MOV and q.op0_register == Register.DX and q.op1_register == Register.CS and dx is None: dx = seg
                if ax is not None and dx is not None and img.lin(dx, ax) not in seen and img.base <= img.lin(dx, ax) < img.end:
                    s2, i2 = L.discover(img, [(dx, ax)])
                    for k2, v in s2.items():
                        if k2 not in seen: seen[k2] = v; added += 1
        if not added: break
    extra = os.path.join(os.path.dirname(out_dir), "..", "tools", "seeds.txt")
    if os.path.exists(extra):
        for line in open(extra):
            line = line.split("#")[0].strip()
            if not line: continue
            s, o = line.split(":")
            s2, i2 = L.discover(img, [(int(s, 16), int(o, 16))])
            for k2, v in s2.items():
                if k2 not in seen: seen[k2] = v
    # Linear sweep over the bytes discovery never reached, assigning them to
    # the segment of the preceding code: unreached routines (other video or
    # sound drivers, rare game paths) still get translated instead of aborting
    # at run time. Data decoded this way is simply never executed.
    lins = sorted(seen)
    covered = bytearray(img.end + 16)
    for l in lins:
        for k in range(seen[l][2].len): covered[l + k] = 1
    swept = 0
    for i, l in enumerate(lins):
        seg, off, ins = seen[l]
        start = l + ins.len
        nxt = lins[i + 1] if i + 1 < len(lins) else img.end
        if start >= nxt or covered[start]: continue
        nseg = seen[nxt][0] if nxt in seen else seg
        if nseg != seg or nxt - start > 0x2000: continue
        dec = Decoder(16, bytes(img.mem[start:nxt]), ip=(start - seg * 16) & 0xFFFF)
        for ins2 in dec:
            lin2 = seg * 16 + ins2.ip
            if lin2 + ins2.len > nxt or ins2.is_invalid: break
            seen[lin2] = (seg, ins2.ip, ins2); swept += 1
    print("swept %d extra instructions from gaps" % swept)
    # group by cs
    fns = {}
    for lin, (seg, off, ins) in seen.items():
        fns.setdefault(seg, Fn(seg)).ins[lin] = ins
    os.makedirs(out_dir, exist_ok=True)
    errors = collections.Counter()
    names = []
    total = 0
    for cs in sorted(fns):
        fn = fns[cs]
        em = Emitter(img, fn)
        lins = sorted(fn.ins)
        base = lins[0]
        end = max(l + fn.ins[l].len for l in lins)
        size = end - base
        name = "seg_%04x" % cs
        names.append((name, cs, base, end, lins))
        out = []
        out.append('#include "cpu.h"')
        out.append("#define NEAR_DISPATCH(x) { uint32_t t_ = (x); uint32_t o_ = t_ - 0x%Xu; if (o_ < 0x%Xu && tab[o_]) goto *tab[o_]; return t_; }" % (base, size))
        out.append("uint32_t %s(uint32_t entry)" % name)
        out.append("{")
        out.append("\tstatic const void* const tab[0x%X] = {" % size)
        out.append(",\n".join("\t\t[0x%X] = &&L_%05x" % (l - base, l) for l in lins))
        out.append("\t};")
        out.append("\t{ uint32_t o_ = entry - 0x%Xu; if (o_ >= 0x%Xu || !tab[o_]) return 0x80000000u | entry; goto *tab[o_]; }" % (base, size))
        for idx_l, l in enumerate(lins):
            if idx_l > 0:
                prev = lins[idx_l - 1]; pins = fn.ins[prev]
                if prev + pins.len != l and pins.flow_control in (FlowControl.NEXT, FlowControl.CONDITIONAL_BRANCH, FlowControl.CALL, FlowControl.INDIRECT_CALL, FlowControl.INTERRUPT):
                    em.emit("return 0x%X;   /* fall-through leaves translated code */" % (prev + pins.len))
            n = len(em.lines)
            try:
                em.translate(l, fn.ins[l])
            except (KeyError, ValueError) as e:
                ins = fn.ins[l]
                del em.lines[n:]
                em.emit("%s: TRACE(0x%05X);" % (em.label(l), l))
                em.emit('fatal("untranslatable instruction at %04x:%04x: %s");' % (cs, ins.ip, fmt.format(ins).replace('"', "'")))
                em.errors["(untranslatable) " + Mnemonic.__dict__.get('x', '') + fmt.format(ins).split()[0]] += 1
        out.extend(em.lines)
        out.append('\tfatal("%s: fell off the end"); return 0;' % name)
        out.append("}")
        write_if_changed(os.path.join(out_dir, name + ".c"), "\n".join(out) + "\n")
        errors.update(em.errors)
        total += len(lins)
    # tables
    t = ['#include "cpu.h"', "#include <stddef.h>"]
    t.append("typedef uint32_t (*SegFn)(uint32_t);")
    for name, cs, base, end, lins in names: t.append("uint32_t %s(uint32_t);" % name)
    t.append("const struct { SegFn fn; uint16_t cs; uint32_t base, end; } seg_table[] = {")
    for name, cs, base, end, lins in names: t.append("\t{ %s, 0x%X, 0x%Xu, 0x%Xu }," % (name, cs, base, end))
    t.append("};")
    t.append("const int seg_count = %d;" % len(names))
    # per-instruction ownership: (lin, fn index) as ranges of consecutive instructions
    ranges = []
    for idx, (name, cs, base, end, lins) in enumerate(names):
        start = None; prev_end = None
        for l in lins:
            ln = fns[cs].ins[l].len
            if start is None: start = l; prev_end = l + ln
            elif l == prev_end: prev_end = l + ln
            else: ranges.append((start, prev_end, idx)); start = l; prev_end = l + ln
        ranges.append((start, prev_end, idx))
    t.append("const struct { uint32_t start, end; uint16_t fn; } code_ranges[] = {")
    for s, e, i in sorted(ranges): t.append("\t{ 0x%Xu, 0x%Xu, %d }," % (s, e, i))
    t.append("};")
    t.append("const int code_range_count = %d;" % len(ranges))
    write_if_changed(os.path.join(out_dir, "tables.c"), "\n".join(t) + "\n")
    print("translated %d instructions in %d segment functions" % (total, len(names)))
    if errors:
        print("UNIMPLEMENTED:")
        for k, v in errors.most_common(): print("  ", v, k)

# The analysis (entry points in tools/seeds.txt, the CT-VOICE hook at
# 117D:0000, the configuration layout) was done on this release of the game.
KNOWN_SHA256 = "5cea262f9610657af8b55c10b277ec30ffe99df35294d5f8c9f520a30e029c96"

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: translate.py path/to/historik.exe src/gen")
    import hashlib
    digest = hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest()
    if digest != KNOWN_SHA256:
        print("warning: %s is not the release this port was made for (sha256 %s)." % (sys.argv[1], digest))
        print("         The translation may be incomplete or the game may not run.")
    img = Image(sys.argv[1])
    build(img, sys.argv[2])
