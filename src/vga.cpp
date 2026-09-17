// EGA/VGA emulation: planar memory with latches and write modes, sequencer,
// graphics controller, attribute controller, CRTC start address, DAC, text
// mode, the video BIOS (int 10h) and the frame composer used by the window.
#include "machine.h"
#include "font8x8.h"
#include <cstring>

namespace {

int32 videoLock = 0;
struct Lock { Lock() { while (atomic_test_and_set(&videoLock, 1, 0) != 0) snooze(20); } ~Lock() { atomic_set(&videoLock, 0); } };

uint8_t planes[4][65536];
uint8_t latch[4];
uint8_t seqIndex = 0, seqRegs[8];
uint8_t gcIndex = 0, gcRegs[16];
uint8_t crtcIndex = 0, crtcRegs[32];
uint8_t attrIndex = 0, attrRegs[32]; bool attrFlipFlop = false;
uint8_t dacRead = 0, dacWrite = 0, dacPhase = 0, dacMask = 0xff;
uint8_t dac[256][3];
uint8_t miscOut = 0x63;
uint8_t mode = 3;
bool chain4 = false;
volatile uint32_t dirty = 1;
bigtime_t frameBase = 0;
const bigtime_t kFramePeriod = 1000000 / 70;   // 70 Hz like 320x200 VGA modes
int retracePolls = 0;

const uint8_t kEgaAttr[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f };

void defaultDac()
{
	// First 64 entries: the EGA rgbRGB colour set; rest: the VGA 256-colour ramp.
	for (int i = 0; i < 64; ++i) {
		int r = ((i >> 2) & 1) * 0x2a + ((i >> 5) & 1) * 0x15;
		int gg = ((i >> 1) & 1) * 0x2a + ((i >> 4) & 1) * 0x15;
		int b = (i & 1) * 0x2a + ((i >> 3) & 1) * 0x15;
		dac[i][0] = uint8_t(r); dac[i][1] = uint8_t(gg); dac[i][2] = uint8_t(b);
	}
	for (int i = 64; i < 256; ++i) { dac[i][0] = dac[i][1] = dac[i][2] = 0; }
}

void cgaDac()
{
	defaultDac();
	static const uint8_t cga[16][3] = { {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},{21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63} };
	for (int i = 0; i < 8; ++i) { memcpy(dac[i], cga[i], 3); memcpy(dac[0x10 + i], cga[8 + i], 3); }
}

void mode13Dac()
{
	static const uint8_t base16[16][3] = { {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},{21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63} };
	for (int i = 0; i < 16; ++i) memcpy(dac[i], base16[i], 3);
	static const uint8_t grays[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
	for (int i = 0; i < 16; ++i) dac[16 + i][0] = dac[16 + i][1] = dac[16 + i][2] = grays[i];
	static const uint8_t hue[24][3] = { {0,0,63},{16,0,63},{31,0,63},{47,0,63},{63,0,63},{63,0,47},{63,0,31},{63,0,16},{63,0,0},{63,16,0},{63,31,0},{63,47,0},{63,63,0},{47,63,0},{31,63,0},{16,63,0},{0,63,0},{0,63,16},{0,63,31},{0,63,47},{0,63,63},{0,47,63},{0,31,63},{0,16,63} };
	static const int scale[3] = { 63, 28, 16 };
	static const int sat[3][2] = { {0, 63}, {31, 63}, {45, 63} };
	int idx = 32;
	for (int s = 0; s < 3; ++s) for (int sa = 0; sa < 3; ++sa) for (int h = 0; h < 24; ++h) {
		for (int c = 0; c < 3; ++c) {
			int v = hue[h][c];
			v = sat[sa][0] + v * (sat[sa][1] - sat[sa][0]) / 63;   // desaturate toward grey
			v = v * scale[s] / 63;
			dac[idx][c] = uint8_t(v);
		}
		idx++;
	}
	for (; idx < 256; ++idx) dac[idx][0] = dac[idx][1] = dac[idx][2] = 0;
}

inline uint8_t rotate(uint8_t v, int n) { n &= 7; return uint8_t((v >> n) | (v << (8 - n))); }

void planarWrite(uint32_t off, uint8_t val)
{
	uint8_t mapMask = seqRegs[2] & 0x0f;
	uint8_t wmode = gcRegs[5] & 3;
	uint8_t bitMask = gcRegs[8];
	uint8_t func = (gcRegs[3] >> 3) & 3;
	uint8_t setReset = gcRegs[0], enableSR = gcRegs[1];
	uint8_t out[4];
	if (wmode == 0) {
		uint8_t r = rotate(val, gcRegs[3] & 7);
		for (int p = 0; p < 4; ++p) {
			uint8_t src = (enableSR >> p) & 1 ? ((setReset >> p) & 1 ? 0xff : 0x00) : r;
			switch (func) { case 1: src &= latch[p]; break; case 2: src |= latch[p]; break; case 3: src ^= latch[p]; break; }
			out[p] = uint8_t((src & bitMask) | (latch[p] & ~bitMask));
		}
	} else if (wmode == 1) {
		for (int p = 0; p < 4; ++p) out[p] = latch[p];
	} else if (wmode == 2) {
		for (int p = 0; p < 4; ++p) {
			uint8_t src = (val >> p) & 1 ? 0xff : 0x00;
			switch (func) { case 1: src &= latch[p]; break; case 2: src |= latch[p]; break; case 3: src ^= latch[p]; break; }
			out[p] = uint8_t((src & bitMask) | (latch[p] & ~bitMask));
		}
	} else {
		uint8_t m = uint8_t(rotate(val, gcRegs[3] & 7) & bitMask);
		for (int p = 0; p < 4; ++p) {
			uint8_t src = (setReset >> p) & 1 ? 0xff : 0x00;
			switch (func) { case 1: src &= latch[p]; break; case 2: src |= latch[p]; break; case 3: src ^= latch[p]; break; }
			out[p] = uint8_t((src & m) | (latch[p] & ~m));
		}
	}
	for (int p = 0; p < 4; ++p) if ((mapMask >> p) & 1) planes[p][off] = out[p];
}

uint8_t planarRead(uint32_t off)
{
	for (int p = 0; p < 4; ++p) latch[p] = planes[p][off];
	if (gcRegs[5] & 8) {
		uint8_t cmp = gcRegs[2] & 0x0f, dontCare = gcRegs[7] & 0x0f, res = 0xff;
		for (int p = 0; p < 4; ++p) if ((dontCare >> p) & 1) res &= uint8_t((cmp >> p) & 1 ? latch[p] : ~latch[p]);
		return res;
	}
	return latch[gcRegs[4] & 3];
}

void setModeRegs(uint8_t m)
{
	memset(seqRegs, 0, sizeof seqRegs); memset(gcRegs, 0, sizeof gcRegs); memset(crtcRegs, 0, sizeof crtcRegs); memset(attrRegs, 0, sizeof attrRegs);
	seqRegs[2] = 0x0f; seqRegs[4] = 0x06; gcRegs[8] = 0xff;
	for (int i = 0; i < 16; ++i) attrRegs[i] = kEgaAttr[i];
	attrRegs[0x10] = 0x01; attrRegs[0x12] = 0x0f;
	chain4 = false;
	switch (m) {
		case 0x13: chain4 = true; seqRegs[4] = 0x0e; gcRegs[5] = 0x40; gcRegs[6] = 0x05; attrRegs[0x10] = 0x41; for (int i = 0; i < 16; ++i) attrRegs[i] = uint8_t(i); crtcRegs[0x13] = 40; break;
		case 0x0D: gcRegs[6] = 0x05; crtcRegs[0x13] = 20; break;
		case 0x0E: gcRegs[6] = 0x05; crtcRegs[0x13] = 40; break;
		case 0x10: gcRegs[6] = 0x05; crtcRegs[0x13] = 40; break;
		case 0x12: gcRegs[6] = 0x05; crtcRegs[0x13] = 40; break;
		case 0x04: case 0x05: case 0x06: gcRegs[6] = 0x0f; crtcRegs[0x13] = 40; break;
		default: gcRegs[6] = 0x0e; seqRegs[4] = 0x02; crtcRegs[0x13] = 40; break;   // text
	}
	// 200-line modes on a VGA use DAC entries 00-07 and 10-17h for the 16 colours.
	if (m == 0x0D || m == 0x0E || m == 0x04 || m == 0x05 || m == 0x06) for (int i = 0; i < 16; ++i) attrRegs[i] = uint8_t(i < 8 ? i : i + 8);
	if (m == 0x13) for (int i = 0; i < 16; ++i) attrRegs[i] = uint8_t(i);
}

void clearVideo()
{
	memset(planes, 0, sizeof planes);
	for (uint32_t a = 0xB8000; a < 0xC0000; a += 2) { mem[a] = ' '; mem[a + 1] = 0x07; }
	memset(mem + 0xB0000, 0, 0x8000);
}

// ---- text helpers ----
uint32_t textCell(int row, int col) { return 0xB8000 + uint32_t(row) * 160 + uint32_t(col) * 2; }

} // namespace

void vga_init()
{
	Lock l;
	defaultDac();
	setModeRegs(3);
	clearVideo();
	mode = 3; frameBase = system_time(); dirty = 1;
}

uint8_t vga_get_mode() { return mode; }

void vga_set_mode(uint8_t m)
{
	Lock l;
	bool noClear = (m & 0x80) != 0; m &= 0x7f;
	tracef("vga: set mode %02x", m);
	mode = m;
	setModeRegs(m);
	if (m == 0x13) mode13Dac(); else if (m == 0x0D || m == 0x0E || m == 0x04 || m == 0x05 || m == 0x06) cgaDac(); else defaultDac();
	if (!noClear) clearVideo();
	uint16_t cols = (m == 0 || m == 1 || m == 4 || m == 5 || m == 0x0D || m == 0x13) ? 40 : 80;
	uint16_t pageSize = cols == 40 ? 2000 : 4000;
	switch (m) { case 0x0D: pageSize = 0x2000; break; case 0x0E: pageSize = 0x4000; break; case 0x10: pageSize = 0x7000; break; case 0x12: pageSize = 0x9600; break; case 0x13: pageSize = 0xFA00; break; case 4: case 5: case 6: pageSize = 0x4000; break; }
	mem[0x449] = m; wr16(0x44A, cols); wr16(0x44C, pageSize); wr16(0x44E, 0); mem[0x462] = 0;
	for (int i = 0; i < 8; ++i) wr16(0x450 + i * 2, 0);
	mem[0x484] = (m == 0x10 || m == 0x12) ? (m == 0x12 ? 29 : 24) : 24; wr16(0x485, (m == 0x0D || m == 0x0E || m == 0x13 || m == 4 || m == 5 || m == 6) ? 8 : (m == 0x10 ? 14 : 16));
	dirty = 1;
}

extern "C" uint8_t vga_read8(uint32_t a)
{
	if (a >= 0xB0000) return mem[a];
	uint32_t off = a - 0xA0000;
	if (chain4) return planes[off & 3][off >> 2];
	return planarRead(off);
}

extern "C" void vga_write8(uint32_t a, uint8_t v)
{
	dirty = 1;
	if (a >= 0xB0000) { mem[a] = v; return; }
	uint32_t off = a - 0xA0000;
	if (chain4) { planes[off & 3][off >> 2] = v; return; }
	planarWrite(off, v);
}

bool vga_port_out(uint16_t port, uint8_t v)
{
	switch (port) {
		case 0x3C4: seqIndex = v & 7; return true;
		case 0x3C5: seqRegs[seqIndex] = v; if (seqIndex == 4) chain4 = (v & 8) != 0; return true;
		case 0x3CE: gcIndex = v & 15; return true;
		case 0x3CF: gcRegs[gcIndex] = v; return true;
		case 0x3D4: case 0x3B4: crtcIndex = v & 31; return true;
		case 0x3D5: case 0x3B5: crtcRegs[crtcIndex] = v; dirty = 1; return true;
		case 0x3C0:
			if (!attrFlipFlop) attrIndex = v & 0x3f; else { attrRegs[attrIndex & 0x1f] = v; dirty = 1; }
			attrFlipFlop = !attrFlipFlop; return true;
		case 0x3C2: miscOut = v; return true;
		case 0x3C6: dacMask = v; return true;
		case 0x3C7: dacRead = v; dacPhase = 0; return true;
		case 0x3C8: dacWrite = v; dacPhase = 0; return true;
		case 0x3C9: { Lock l; dac[dacWrite][dacPhase] = v & 0x3f; if (++dacPhase == 3) { dacPhase = 0; dacWrite++; } dirty = 1; return true; }
		case 0x3B8: case 0x3BF: case 0x3D8: case 0x3D9: case 0x3DA: case 0x3BA: return true;
		default: return false;
	}
}

bool vga_port_in(uint16_t port, uint8_t& v)
{
	switch (port) {
		case 0x3C4: v = seqIndex; return true;
		case 0x3C5: v = seqRegs[seqIndex]; return true;
		case 0x3CE: v = gcIndex; return true;
		case 0x3CF: v = gcRegs[gcIndex]; return true;
		case 0x3D4: case 0x3B4: v = crtcIndex; return true;
		case 0x3D5: case 0x3B5: v = crtcRegs[crtcIndex]; return true;
		case 0x3C0: v = attrIndex; return true;
		case 0x3C1: v = attrRegs[attrIndex & 0x1f]; return true;
		case 0x3C2: v = 0x10; return true;
		case 0x3CC: v = miscOut; return true;
		case 0x3C6: v = dacMask; return true;
		case 0x3C7: v = 0; return true;
		case 0x3C8: v = dacWrite; return true;
		case 0x3C9: v = dac[dacRead][dacPhase]; if (++dacPhase == 3) { dacPhase = 0; dacRead++; } return true;
		case 0x3DA: case 0x3BA: {
			attrFlipFlop = false;
			bigtime_t t = (system_time() - frameBase) % kFramePeriod;
			bool vretrace = t < 1200;                         // ~8% of the frame
			bool hblank = ((system_time() / 8) & 3) == 0;
			v = uint8_t((vretrace ? 0x08 : 0) | ((vretrace || hblank) ? 0x01 : 0));
			// A game polling for the retrace edge would otherwise spin a core.
			if (++retracePolls > 8) { snooze(150); retracePolls = 0; }
			return true;
		}
		case 0x3B8: case 0x3D8: case 0x3D9: v = 0; return true;
		default: return false;
	}
}

// ---- BIOS int 10h ----
void vga_text_scroll(int dir, uint8_t attr, int top, int left, int bottom, int right, int lines)
{
	if (bottom > 24) bottom = 24; if (right > 79) right = 79;
	int h = bottom - top + 1;
	if (lines == 0 || lines >= h) {
		for (int r = top; r <= bottom; ++r) for (int c = left; c <= right; ++c) { uint32_t a = textCell(r, c); mem[a] = ' '; mem[a + 1] = attr; }
	} else if (dir > 0) {
		for (int r = top; r <= bottom - lines; ++r) for (int c = left; c <= right; ++c) { uint32_t d = textCell(r, c), s = textCell(r + lines, c); mem[d] = mem[s]; mem[d + 1] = mem[s + 1]; }
		for (int r = bottom - lines + 1; r <= bottom; ++r) for (int c = left; c <= right; ++c) { uint32_t a = textCell(r, c); mem[a] = ' '; mem[a + 1] = attr; }
	} else {
		for (int r = bottom; r >= top + lines; --r) for (int c = left; c <= right; ++c) { uint32_t d = textCell(r, c), s = textCell(r - lines, c); mem[d] = mem[s]; mem[d + 1] = mem[s + 1]; }
		for (int r = top; r < top + lines; ++r) for (int c = left; c <= right; ++c) { uint32_t a = textCell(r, c); mem[a] = ' '; mem[a + 1] = attr; }
	}
	dirty = 1;
}

void vga_bios_int10()
{
	switch (AH) {
		case 0x00: vga_set_mode(AL); break;
		case 0x01: wr16(0x460, rCX); dirty = 1; break;
		case 0x02: if (BH < 8) wr16(0x450 + BH * 2, rDX); dirty = 1; break;
		case 0x03: rDX = rd16(0x450 + (BH & 7) * 2); rCX = rd16(0x460); break;
		case 0x05: {
			// Select the displayed page: the CRTC start address moves to that page.
			if (AL > 7) break;
			mem[0x462] = AL;
			uint32_t start = uint32_t(AL) * rd16(0x44C);
			if (mode == 0x0D || mode == 0x0E || mode == 0x10 || mode == 0x12) { Lock l; crtcRegs[0x0C] = uint8_t(start >> 8); crtcRegs[0x0D] = uint8_t(start); }
			wr16(0x44E, uint16_t(start)); dirty = 1;
			break;
		}
		case 0x06: vga_text_scroll(1, BH, CH, CL, DH, DL, AL); break;
		case 0x07: vga_text_scroll(-1, BH, CH, CL, DH, DL, AL); break;
		case 0x08: { uint16_t pos = rd16(0x450 + (BH & 7) * 2); uint32_t a = textCell(pos >> 8, pos & 0xff); AL = mem[a]; AH = mem[a + 1]; break; }
		case 0x09: case 0x0A: {
			uint16_t pos = rd16(0x450 + (BH & 7) * 2); int row = pos >> 8, col = pos & 0xff;
			for (int i = 0; i < rCX && col < 80; ++i, ++col) { uint32_t a = textCell(row, col); mem[a] = AL; if (AH == 0x09) mem[a + 1] = BL; }
			dirty = 1; break;
		}
		case 0x0E: {
			uint16_t pos = rd16(0x450); int row = pos >> 8, col = pos & 0xff;
			if (AL == 13) col = 0; else if (AL == 10) row++; else if (AL == 8) { if (col > 0) col--; } else if (AL == 7) {} else { uint32_t a = textCell(row, col); mem[a] = AL; col++; if (col >= 80) { col = 0; row++; } }
			if (row >= 25) { vga_text_scroll(1, 0x07, 0, 0, 24, 79, 1); row = 24; }
			wr16(0x450, uint16_t((row << 8) | col)); dirty = 1; break;
		}
		case 0x0B: break;
		case 0x0F: AL = mem[0x449]; AH = uint8_t(rd16(0x44A)); BH = mem[0x462]; break;
		case 0x10:
			switch (AL) {
				case 0x00: { Lock l; attrRegs[BL & 0x1f] = BH; dirty = 1; break; }
				case 0x01: attrRegs[0x11] = BH; break;
				case 0x02: { Lock l; uint32_t a = LIN(rES, rDX); for (int i = 0; i < 16; ++i) attrRegs[i] = mem[a + i]; attrRegs[0x11] = mem[a + 16]; dirty = 1; break; }
				case 0x03: break;
				case 0x07: BH = attrRegs[BL & 0x1f]; break;
				case 0x09: { uint32_t a = LIN(rES, rDX); for (int i = 0; i < 16; ++i) mem[a + i] = attrRegs[i]; mem[a + 16] = attrRegs[0x11]; break; }
				case 0x10: { Lock l; dac[BL][0] = DH & 0x3f; dac[BL][1] = CH & 0x3f; dac[BL][2] = CL & 0x3f; dirty = 1; break; }
				case 0x12: { Lock l; uint32_t a = LIN(rES, rDX); for (int i = 0; i < rCX && BL + i < 256; ++i) { dac[(rBX + i) & 0xff][0] = mem[a + i * 3] & 0x3f; dac[(rBX + i) & 0xff][1] = mem[a + i * 3 + 1] & 0x3f; dac[(rBX + i) & 0xff][2] = mem[a + i * 3 + 2] & 0x3f; } dirty = 1; break; }
				case 0x15: DH = dac[BL][0]; CH = dac[BL][1]; CL = dac[BL][2]; break;
				case 0x17: { uint32_t a = LIN(rES, rDX); for (int i = 0; i < rCX; ++i) { mem[a + i * 3] = dac[(rBX + i) & 0xff][0]; mem[a + i * 3 + 1] = dac[(rBX + i) & 0xff][1]; mem[a + i * 3 + 2] = dac[(rBX + i) & 0xff][2]; } break; }
				case 0x1A: BL = 0; BH = 0; break;
				default: tracef("bios: int10 AX=%04x unhandled", rAX); break;
			}
			break;
		case 0x11:
			if (AL == 0x30) { rES = 0xF000; rBP = 0xFA6E; rCX = 8; DL = 24; }
			break;
		case 0x12:
			if (BL == 0x10) { BH = 0; BL = 3; CH = 0; CL = 9; }
			else if (BL == 0x30 || BL == 0x31 || BL == 0x32 || BL == 0x33 || BL == 0x34 || BL == 0x36) AL = 0x12;
			break;
		case 0x1A: if (AL == 0) { AL = 0x1A; BL = 8; BH = 0; } break;
		case 0x1B: AL = 0; break;
		case 0x13: {
			uint32_t a = LIN(rES, rBP); int row = DH, col = DL;
			for (int i = 0; i < rCX; ++i) { uint8_t c = mem[a + ((AL & 2) ? i * 2 : i)]; uint8_t at = (AL & 2) ? mem[a + i * 2 + 1] : BL; if (c == 13) { col = 0; continue; } if (c == 10) { row++; continue; } uint32_t d = textCell(row, col); mem[d] = c; mem[d + 1] = at; if (++col >= 80) { col = 0; row++; } }
			if (AL & 1) wr16(0x450, uint16_t((row << 8) | col));
			dirty = 1; break;
		}
		case 0xFE: case 0xFF: break;
		default: tracef("bios: int10 AH=%02x unhandled", AH); break;
	}
}

// ---- frame composition ----
uint64_t vga_frame_count = 0;

bool vga_compose(uint32_t* out, int& w, int& h)
{
	Lock l;
	++vga_frame_count;
	static uint32_t lastStart = 0xffffffff;
	uint16_t start = uint16_t((crtcRegs[0x0C] << 8) | crtcRegs[0x0D]);
	if (!dirty && start == lastStart) return false;
	dirty = 0; lastStart = start;
	uint32_t pal[256];
	for (int i = 0; i < 256; ++i) pal[i] = 0xff000000u | (uint32_t(dac[i][0] * 255 / 63) << 16) | (uint32_t(dac[i][1] * 255 / 63) << 8) | uint32_t(dac[i][2] * 255 / 63);
	if (mode == 0x0D || mode == 0x0E || mode == 0x10 || mode == 0x12) {
		w = (mode == 0x0D) ? 320 : 640; h = (mode == 0x10) ? 350 : (mode == 0x12) ? 480 : 200;
		int pitch = crtcRegs[0x13] ? crtcRegs[0x13] * 2 : w / 8;
		int pan = attrRegs[0x13] & 7;
		bool blink = (attrRegs[0x10] & 8) != 0;
		for (int y = 0; y < h; ++y) {
			uint32_t rowOff = start + uint32_t(y) * pitch;
			for (int x = 0; x < w; ++x) {
				int px = x + pan;
				uint32_t off = (rowOff + (px >> 3)) & 0xffff; int bit = 7 - (px & 7);
				int idx = ((planes[0][off] >> bit) & 1) | (((planes[1][off] >> bit) & 1) << 1) | (((planes[2][off] >> bit) & 1) << 2) | (((planes[3][off] >> bit) & 1) << 3);
				uint8_t a = attrRegs[idx & 15];
				int dacIdx = blink ? ((a & 0x0f) | ((attrRegs[0x14] & 0x0f) << 4)) : ((a & 0x3f) | ((attrRegs[0x14] & 0x0c) << 4));
				out[y * w + x] = pal[dacIdx & 0xff];
			}
		}
		return true;
	}
	if (mode == 0x13) {
		w = 320; h = 200;
		for (int y = 0; y < 200; ++y) for (int x = 0; x < 320; ++x) {
			uint32_t off = (start * 4 + uint32_t(y) * 320 + x) & 0xffff;
			out[y * 320 + x] = pal[planes[off & 3][off >> 2]];
		}
		return true;
	}
	if (mode == 0x04 || mode == 0x05 || mode == 0x06) {
		w = 320; h = 200;
		for (int y = 0; y < 200; ++y) for (int x = 0; x < 320; ++x) {
			uint32_t a = 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80;
			int idx;
			if (mode == 6) { uint8_t b = mem[a + (x >> 3)]; idx = ((b >> (7 - (x & 7))) & 1) ? 15 : 0; }
			else { uint8_t b = mem[a + (x >> 2)]; int c = (b >> (6 - 2 * (x & 3))) & 3; static const int cgaPal[4] = { 0, 3, 5, 7 }; idx = c ? cgaPal[c] + 8 : 0; }
			out[y * 320 + x] = pal[kEgaAttr[idx]];
		}
		return true;
	}
	// text mode 80x25 with the 8x8 font doubled vertically to 8x16
	w = 640; h = 400;
	uint32_t base = (mode == 7) ? 0xB0000 : 0xB8000;
	uint16_t cur = rd16(0x450); int curRow = cur >> 8, curCol = cur & 0xff;
	uint16_t shape = rd16(0x460); bool cursorOn = ((shape >> 8) & 0x20) == 0 && (shape >> 8) <= (shape & 0xff);
	bool cursorBlink = ((system_time() / 250000) & 1) == 0;
	for (int row = 0; row < 25; ++row) for (int col = 0; col < 80; ++col) {
		uint32_t a = base + uint32_t(row) * 160 + uint32_t(col) * 2;
		uint8_t ch = mem[a], at = mem[a + 1];
		uint32_t fg = pal[kEgaAttr[at & 15]], bg = pal[kEgaAttr[(at >> 4) & 7]];
		const unsigned char* gl = font8x8[ch];
		for (int gy = 0; gy < 16; ++gy) {
			uint8_t bits = gl[gy >> 1];
			bool curLine = cursorOn && cursorBlink && row == curRow && col == curCol && (gy >> 1) >= ((shape >> 8) & 0x1f) / 2 && (gy >> 1) <= (shape & 0x1f) / 2;
			uint32_t* dst = out + (row * 16 + gy) * 640 + col * 8;
			for (int gx = 0; gx < 8; ++gx) dst[gx] = ((bits >> gx) & 1) || curLine ? fg : bg;
		}
	}
	dirty = 1;   // keep the cursor blinking
	return true;
}
