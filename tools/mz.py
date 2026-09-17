"""MZ loader + recursive-descent discovery for the 16-bit Prehistorik image."""
import struct, sys, json
from iced_x86 import Decoder, Instruction, FlowControl, Code, OpKind, Register, Mnemonic, Formatter, FormatterSyntax

LOADSEG = 0x0100  # image base segment (PSP at 0x00F0)
PSPSEG = LOADSEG - 0x10

class Image:
    def __init__(self, path):
        d = open(path, "rb").read()
        (sig, lastpage, pages, nreloc, hdrpara, minal, maxal, ss, sp, csum, ip, cs, reloff, ovl) = struct.unpack("<2sHHHHHHHHHHHHH", d[:28])
        hdr = hdrpara * 16
        size = (pages - 1) * 512 + lastpage if lastpage else pages * 512
        self.raw = bytearray(d[hdr:size])
        self.relocs = [struct.unpack("<HH", d[reloff + i * 4:reloff + i * 4 + 4]) for i in range(nreloc)]
        self.cs, self.ip, self.ss, self.sp, self.minal = cs, ip, ss, sp, minal
        self.reloc_lin = set()
        for o, s in self.relocs:
            lin = s * 16 + o
            v = struct.unpack_from("<H", self.raw, lin)[0]
            struct.pack_into("<H", self.raw, lin, (v + LOADSEG) & 0xFFFF)
            self.reloc_lin.add(lin)
        self.mem = bytearray(0x110000)
        self.base = LOADSEG * 16
        self.mem[self.base:self.base + len(self.raw)] = self.raw
        self.end = self.base + len(self.raw)

    def lin(self, seg, off):
        return (seg * 16 + off) & 0xFFFFF

    def word(self, lin):
        return struct.unpack_from("<H", self.mem, lin)[0]

fmt = Formatter(FormatterSyntax.NASM)

def decode_at(img, seg, off):
    lin = img.lin(seg, off)
    dec = Decoder(16, bytes(img.mem[lin:lin + 16]), ip=off)
    ins = dec.decode()
    return ins

if __name__ == "__main__":
    img = Image(sys.argv[1])
    print("image %05x-%05x cs:ip=%04x:%04x" % (img.base, img.end, img.cs + LOADSEG, img.ip))
