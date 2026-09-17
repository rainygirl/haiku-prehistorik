// Native runtime for the translated Prehistorik code: machine state, the
// dispatcher that chains segment functions, interrupt delivery, the DOS and
// BIOS services the game expects, the PIT and the keyboard controller.
#include "machine.h"
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

extern "C" {
uint16_t rAX, rBX, rCX, rDX, rSI, rDI, rBP, rSP, rCS, rDS, rES, rSS;
uint8_t fCF, fPF, fAF, fZF, fSF, fTF, fIF, fDF, fOF;
uint8_t mem[MEM_SIZE];
volatile int32_t irq_pending;
uint32_t spin_count;
const uint8_t parity_table[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
	P6(1), P6(0), P6(0), P6(1)
};
typedef uint32_t (*SegFn)(uint32_t);
extern const struct { SegFn fn; uint16_t cs; uint32_t base, end; } seg_table[];
extern const int seg_count;
extern const struct { uint32_t start, end; uint16_t fn; } code_ranges[];
extern const int code_range_count;
}

Globals g;

#define IRQP ((int32*)(void*)&irq_pending)

namespace {

const uint16_t kPspSeg = 0x00F0;
const uint16_t kLoadSeg = 0x0100;
const uint16_t kEnvSeg = 0x00E0;
const uint16_t kTopSeg = 0xA000;
const uint16_t kNativeSeg = 0xF000;     // IVT entries that are native services: F000:n
const uint16_t kReturnSeg = 0xFFFF;     // return to native caller of run_loop
const uint16_t kStubSeg = 0xFFFE;       // continuation of the Turbo Pascal Intr() stub

std::vector<uint16_t> fnIndex;          // per image byte: segment function index + 1, 0 = none
uint16_t fnByCs[65536];                 // CS value -> segment function index + 1
const uint32_t kNotHere = 0x80000000u;  // returned by a segment function for an unknown entry
jmp_buf exitJump;
uint32_t iretDepth = 0;

// ---- statistics (PREH_STATS=1) ----
bool stats = false;
uint64_t irq0Raised = 0, irq0Delivered = 0, irq1Delivered = 0, framesLast = 0;
bigtime_t statsLast = 0;

// ---- trace ring ----
uint32_t traceRing[65536]; uint32_t traceHead = 0;

// ---- keyboard ----
uint8_t keyQueue[256]; volatile int keyHead = 0, keyTail = 0;
uint8_t currentScancode = 0;
uint16_t biosKeys[32]; int biosHead = 0, biosTail = 0;
bool shiftDown = false, ctrlDown = false, altDown = false;
int32 keyLock = 0;
struct SpinLock { int32* l; SpinLock(int32* x) : l(x) { while (atomic_test_and_set(l, 1, 0) != 0) snooze(20); } ~SpinLock() { atomic_set(l, 0); } };

// Haiku raw key codes -> PC set 1 make codes (0x100 = E0 prefix).
const uint16_t kScan[0x70] = {
	0, 0x01, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0x137, 0x46,
	0x145, 0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x152,
	0x147, 0x149, 0x45, 0x135, 0x37, 0x4a, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
	0x19, 0x1a, 0x1b, 0x2b, 0x153, 0x14f, 0x151, 0x47, 0x48, 0x49, 0x4e, 0x3a, 0x1e, 0x1f, 0x20, 0x21,
	0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x1c, 0x4b, 0x4c, 0x4d, 0x2a, 0x2c, 0x2d, 0x2e, 0x2f,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x148, 0x4f, 0x50, 0x51, 0x11c, 0x1d, 0x38, 0x39, 0x138,
	0x11d, 0x14b, 0x150, 0x14d, 0x52, 0x53, 0x15b, 0x15c, 0x15d, 0, 0, 0, 0, 0, 0, 0 };
const char kAscii[128] = { 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ' };
const char kAsciiShift[128] = { 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ' };

void pushKey(uint8_t code) { int next = (keyTail + 1) & 255; if (next == keyHead) return; keyQueue[keyTail] = code; keyTail = next; }
void pushBiosKey(uint16_t sc, bool extended, bool down)
{
	if (sc == 0x2a || sc == 0x36) { shiftDown = down; return; }
	if (sc == 0x1d) { ctrlDown = down; return; }
	if (sc == 0x38) { altDown = down; return; }
	if (!down) return;
	uint8_t ascii = 0;
	if (!extended && sc < 128) ascii = uint8_t(shiftDown ? kAsciiShift[sc] : kAscii[sc]);
	if (ctrlDown && ascii >= 'a' && ascii <= 'z') ascii = uint8_t(ascii - 'a' + 1);
	if (altDown || extended) ascii = 0;
	if (extended && sc == 0x1c) ascii = 13;   // keypad enter
	int next = (biosTail + 1) & 31; if (next == biosHead) return;
	biosKeys[biosTail] = uint16_t((sc << 8) | ascii); biosTail = next;
}
// Keyboard flags at 40:17 follow the modifier state for programs that read them.
void updateBiosShiftFlags()
{
	uint8_t f = 0;
	if (shiftDown) f |= 3; if (ctrlDown) f |= 4; if (altDown) f |= 8;
	mem[0x417] = f;
}

// ---- PIT ----
struct PitChannel { uint16_t reload = 0; uint8_t access = 3, mode = 3, latchPhase = 0; uint16_t latchValue = 0; bool latched = false; bigtime_t start = 0; };
PitChannel pit[3];
int32 timerPendingCount = 0;
uint8_t port61 = 0;
thread_id timerThread = -1;

double pitHz(int ch) { uint32_t d = pit[ch].reload ? pit[ch].reload : 65536; return 1193182.0 / d; }

uint16_t pitCurrentCount(int ch)
{
	uint32_t reload = pit[ch].reload ? pit[ch].reload : 65536;
	bigtime_t el = system_time() - pit[ch].start;
	uint64_t ticks = uint64_t(el) * 1193182 / 1000000;
	uint32_t c = reload - uint32_t(ticks % reload);
	return uint16_t(c & 0xffff);
}

int32 timerLoop(void*)
{
	bigtime_t next = system_time();
	while (!g.quitting) {
		double hz = pitHz(0);
		bigtime_t period = bigtime_t(1000000.0 / hz);
		if (period < 100) period = 100;
		next += period;
		bigtime_t now = system_time();
		if (next < now - 50000) next = now;   // fell far behind (debugger, sleep): resync
		if (next > now) snooze(next - now);
		if (atomic_add(&timerPendingCount, 1) > 16) atomic_add(&timerPendingCount, -1);
		++irq0Raised;
		atomic_or(IRQP, 1);
	}
	return 0;
}

// ---- DOS files ----
struct DosFile { int fd = -1; bool open = false; bool isDevice = false; };
DosFile files[32];
std::string ctVoiceImage;   // synthetic driver image served for ct-voice.drv

std::string lowerBase(const std::string& dosPath)
{
	size_t s = dosPath.find_last_of("\\/:");
	std::string base = s == std::string::npos ? dosPath : dosPath.substr(s + 1);
	for (size_t i = 0; i < base.size(); ++i) base[i] = char(tolower(base[i]));
	return base;
}

std::string resolveHost(const std::string& dosPath, bool forCreate)
{
	std::string base = lowerBase(dosPath);
	if (base.empty()) return "";
	DIR* d = opendir(g.gameDir.c_str());
	if (d) {
		while (dirent* e = readdir(d)) {
			std::string n = e->d_name; std::string l = n;
			for (size_t i = 0; i < l.size(); ++i) l[i] = char(tolower(l[i]));
			if (l == base) { closedir(d); return g.gameDir + "/" + n; }
		}
		closedir(d);
	}
	return forCreate ? g.gameDir + "/" + base : "";
}

std::string readAsciiz(uint32_t lin) { std::string s; while (mem[lin] && s.size() < 128) s.push_back(char(mem[lin++])); return s; }

int allocHandle() { for (int i = 5; i < 32; ++i) if (!files[i].open) return i; return -1; }

void dosError(uint16_t code) { fCF = 1; rAX = code; }
void dosOk() { fCF = 0; }

// CT-VOICE.DRV is not shipped with the game; its functions are native
// (ctvoice_call). The game only checks the "CT-VOICE" signature at offset 3
// before it calls the loaded image, so a small synthetic file is served.
// Each call creates a new file; dosOpen() deletes it right after opening, so
// nothing is left in /tmp.
std::string virtualDriverPath()
{
	char tmpl[] = "/tmp/prehistorik-ctvoice-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) return "";
	uint8_t img[256] = { 0xE9, 0xFD, 0x00, 'C', 'T', '-', 'V', 'O', 'I', 'C', 'E' };
	if (::write(fd, img, sizeof img) != ssize_t(sizeof img)) { ::close(fd); ::unlink(tmpl); return ""; }
	::close(fd);
	return tmpl;
}

// With g.configPath set (headless tools), the game reads its configuration
// from that file and its rewrite of it lands in a scratch file, so the game
// directory is never modified.
std::string configScratchPath()
{
	char tmpl[] = "/tmp/prehistorik-cfg-XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) return "";
	::close(fd);
	return tmpl;
}

bool dosOpen(const std::string& name, int mode, bool create)
{
	std::string host = resolveHost(name, create);
	bool temporary = false;   // a scratch file to delete as soon as it is open
	if (!g.configPath.empty() && lowerBase(name) == "grawaga.cfg") {
		temporary = create || (mode & 3) != 0;
		host = temporary ? configScratchPath() : g.configPath;
	}
	if (host.empty() && !create && lowerBase(name) == "ct-voice.drv") { host = virtualDriverPath(); temporary = !host.empty(); }
	int h = allocHandle();
	if (h < 0) { dosError(4); return true; }
	if (host.empty()) { tracef("dos: open %s -> not found", name.c_str()); dosError(2); return true; }
	int flags = create ? (O_RDWR | O_CREAT | O_TRUNC) : ((mode & 3) == 0 ? O_RDONLY : (mode & 3) == 1 ? O_WRONLY : O_RDWR);
	int fd = ::open(host.c_str(), flags, 0644);
	if (fd < 0) {
		// read-only media / permissions: fall back to read-only
		if (!create) fd = ::open(host.c_str(), O_RDONLY);
		if (fd < 0) { tracef("dos: open %s failed: %s", host.c_str(), strerror(errno)); dosError(create ? 3 : 5); return true; }
	}
	files[h].fd = fd; files[h].open = true; files[h].isDevice = false;
	rAX = uint16_t(h); dosOk();
	tracef("dos: open %s -> handle %d", host.c_str(), h);
	if (temporary) ::unlink(host.c_str());   // the open handle stays valid
	return true;
}

void dos21()
{
	uint8_t ah = AH;
	switch (ah) {
		case 0x30: rAX = 0x0003 | (30 << 8); rBX = 0; rCX = 0; fCF = 0; break;   // DOS 3.30, AL=3 AH=30
		case 0x25: { int n = AL; wr16(n * 4, rDX); wr16(n * 4 + 2, rDS); tracef("dos: set vector %02x = %04x:%04x", n, rDS, rDX); break; }
		case 0x35: { int n = AL; rBX = rd16(n * 4); rES = rd16(n * 4 + 2); break; }
		case 0x4A: { dosOk(); rBX = uint16_t(kTopSeg - rES); break; }
		case 0x48: { dosError(8); rBX = 0; break; }
		case 0x49: dosOk(); break;
		case 0x4C: g.exitCode = AL; tracef("dos: exit %d", AL); longjmp(exitJump, 1); break;
		case 0x00: g.exitCode = 0; longjmp(exitJump, 1); break;
		case 0x19: AL = 2; break;                                  // current drive C:
		case 0x0E: AL = 26; break;
		case 0x47: { mem[LIN(rDS, rSI)] = 0; dosOk(); break; }     // getcwd: root
		case 0x3B: dosOk(); break;                                 // chdir
		case 0x2A: { time_t t = time(NULL); struct tm* tm = localtime(&t); rCX = uint16_t(tm->tm_year + 1900); DH = uint8_t(tm->tm_mon + 1); DL = uint8_t(tm->tm_mday); AL = uint8_t(tm->tm_wday); break; }
		case 0x2C: { bigtime_t us = real_time_clock_usecs(); time_t t = time(NULL); struct tm* tm = localtime(&t); CH = uint8_t(tm->tm_hour); CL = uint8_t(tm->tm_min); DH = uint8_t(tm->tm_sec); DL = uint8_t((us / 10000) % 100); break; }
		case 0x3D: dosOpen(readAsciiz(LIN(rDS, rDX)), AL, false); break;
		case 0x3C: dosOpen(readAsciiz(LIN(rDS, rDX)), 2, true); break;
		case 0x3E: { int h = rBX; if (h >= 0 && h < 32 && files[h].open) { if (h > 4) ::close(files[h].fd); files[h].open = h <= 4; dosOk(); } else dosError(6); break; }
		case 0x3F: {
			int h = rBX;
			if (h < 0 || h >= 32 || !files[h].open) { dosError(6); break; }
			if (h <= 4) { rAX = 0; dosOk(); break; }
			uint32_t dst = LIN(rDS, rDX); uint32_t n = rCX;
			if (dst + n > MEM_SIZE) n = MEM_SIZE - dst;
			ssize_t r = ::read(files[h].fd, mem + dst, n);
			if (r < 0) { dosError(5); break; }
			rAX = uint16_t(r); dosOk();
			break;
		}
		case 0x40: {
			int h = rBX;
			if (h < 0 || h >= 32 || !files[h].open) { dosError(6); break; }
			uint32_t src = LIN(rDS, rDX); uint32_t n = rCX;
			if (h <= 4) { std::string s(reinterpret_cast<char*>(mem + src), n); logf("game: %s", s.c_str()); rAX = uint16_t(n); dosOk(); break; }
			ssize_t r = ::write(files[h].fd, mem + src, n);
			if (r < 0) { dosError(5); break; }
			rAX = uint16_t(r); dosOk();
			break;
		}
		case 0x42: {
			int h = rBX;
			if (h < 0 || h >= 32 || !files[h].open || h <= 4) { dosError(6); break; }
			off_t pos = off_t((int32_t)((uint32_t(rCX) << 16) | rDX));
			off_t r = ::lseek(files[h].fd, pos, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END);
			if (r < 0) { dosError(1); break; }
			rAX = uint16_t(r & 0xffff); rDX = uint16_t((r >> 16) & 0xffff); dosOk();
			break;
		}
		case 0x43: {
			std::string host = resolveHost(readAsciiz(LIN(rDS, rDX)), false);
			if (host.empty()) { dosError(2); break; }
			if (AL == 0) rCX = 0x20;
			dosOk(); break;
		}
		case 0x41: { std::string host = resolveHost(readAsciiz(LIN(rDS, rDX)), false); if (host.empty() || ::unlink(host.c_str()) != 0) dosError(2); else dosOk(); break; }
		case 0x44: {
			int h = rBX;
			if (AL == 0) { if (h >= 0 && h < 32 && files[h].open) { rDX = h <= 4 ? 0x80D3 : 0x0002; rAX = rDX; dosOk(); } else dosError(6); }
			else if (AL == 1) dosOk();
			else dosError(1);
			break;
		}
		case 0x09: { uint32_t a = LIN(rDS, rDX); std::string s; while (mem[a] != '$' && s.size() < 512) s.push_back(char(mem[a++])); logf("game: %s", s.c_str()); break; }
		case 0x02: case 0x06: { char c = char(DL); if (ah == 0x02 || c != char(0xff)) logf("game char: %c", c); AL = uint8_t(c); break; }
		case 0x0B: AL = 0; break;
		case 0x33: if (AL == 0) DL = 0; break;
		case 0x34: rES = 0; rBX = 0x100; break;
		case 0x37: AL = 0; DL = '/'; break;
		case 0x38: dosOk(); break;
		case 0x4E: case 0x4F: dosError(18); break;
		case 0x56: dosError(2); break;
		case 0x57: rCX = 0; rDX = 0; dosOk(); break;
		case 0x5A: case 0x5B: dosError(3); break;
		case 0x62: rBX = kPspSeg; break;
		case 0x2F: rES = kPspSeg; rBX = 0x80; break;
		case 0x1A: break;
		default: tracef("dos: unhandled int 21h AH=%02x AL=%02x", ah, AL); dosError(1); break;
	}
}

void bios16()
{
	switch (AH) {
		case 0x00: case 0x10:
			while (biosHead == biosTail) { snooze(5000); if (g.quitting) longjmp(exitJump, 1); }
			rAX = biosKeys[biosHead]; biosHead = (biosHead + 1) & 31; break;
		case 0x01: case 0x11:
			if (biosHead == biosTail) { fZF = 1; snooze(1000); } else { fZF = 0; rAX = biosKeys[biosHead]; }
			break;
		case 0x02: case 0x12: AL = mem[0x417]; if (AH == 0x12) AH = 0; break;
		case 0x05: { int next = (biosTail + 1) & 31; if (next != biosHead) { biosKeys[biosTail] = rCX; biosTail = next; AL = 0; } else AL = 1; break; }
		default: tracef("bios: unhandled int 16h AH=%02x", AH); break;
	}
}

void nativeInt(int n)
{
	switch (n) {
		case 0x08: {
			uint32_t t = rd16(0x46C) | (uint32_t(rd16(0x46E)) << 16);
			t++; if (t >= 0x1800B0) { t = 0; mem[0x470] = 1; }
			wr16(0x46C, uint16_t(t)); wr16(0x46E, uint16_t(t >> 16));
			call_int_nested(0x1C);
			break;
		}
		case 0x09: break;
		case 0x1C: case 0x23: case 0x24: case 0x1B: break;
		case 0x00: fatal("divide error at %04x", rCS); break;
		case 0x10: vga_bios_int10(); break;
		case 0x11: rAX = rd16(0x410); break;
		case 0x12: rAX = 640; break;
		case 0x15: fCF = 1; AH = 0x86; break;
		case 0x16: bios16(); break;
		case 0x1A: if (AH == 0) { uint32_t t = rd16(0x46C) | (uint32_t(rd16(0x46E)) << 16); rCX = uint16_t(t >> 16); rDX = uint16_t(t); AL = mem[0x470]; mem[0x470] = 0; } else if (AH == 2) { time_t tt = time(NULL); struct tm* tm = localtime(&tt); CH = uint8_t(((tm->tm_hour / 10) << 4) | (tm->tm_hour % 10)); CL = uint8_t(((tm->tm_min / 10) << 4) | (tm->tm_min % 10)); DH = uint8_t(((tm->tm_sec / 10) << 4) | (tm->tm_sec % 10)); fCF = 0; } break;
		case 0x21: dos21(); break;
		case 0x33: if (rAX == 0) { rAX = 0; rBX = 0; } else if (rAX == 3) { rBX = 0; rCX = 0; rDX = 0; } break;
		case 0x2F: break;
		default: tracef("bios: unhandled int %02x AX=%04x", n, rAX); break;
	}
}

} // namespace

// ---- logging ----
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }
void tracef(const char* fmt, ...)
{
	if (!g.trace) return;
	FILE* f = g.traceFile ? g.traceFile : stderr;
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); fputc('\n', f); va_end(ap);
}

extern "C" void trace_ins(uint32_t lin) { traceRing[traceHead++ & 65535] = lin; }

extern "C" void fatal(const char* fmt, ...)
{
	va_list ap; va_start(ap, fmt); fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
	fprintf(stderr, "regs: AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x BP=%04x SP=%04x CS=%04x DS=%04x ES=%04x SS=%04x\n", rAX, rBX, rCX, rDX, rSI, rDI, rBP, rSP, rCS, rDS, rES, rSS);
	fprintf(stderr, "stack:"); for (int i = 0; i < 16; ++i) fprintf(stderr, " %04x", rd16(LIN(rSS, rSP + i * 2))); fprintf(stderr, "\n");
	if (traceHead) { fprintf(stderr, "last instructions:"); for (uint32_t i = traceHead > 40 ? traceHead - 40 : 0; i < traceHead; ++i) fprintf(stderr, " %05x", traceRing[i & 65535]); fprintf(stderr, "\n"); }
	if (g.traceFile) fflush(g.traceFile);
	g.quitting = true;
	longjmp(exitJump, 2);
}

extern "C" void div_zero(void) { call_int_nested(0); }

// ---- dispatcher ----
uint32_t code_owner_lookup(uint32_t lin)
{
	if (lin < IMG_BASE || lin - IMG_BASE >= fnIndex.size()) return 0;
	return fnIndex[lin - IMG_BASE];
}

void run_loop(uint32_t lin)
{
	for (;;) {
		if (rCS == kReturnSeg) return;
		if (rCS == kNativeSeg) {
			int n = int(lin - (uint32_t(kNativeSeg) << 4));
			nativeInt(n);
			uint16_t ip = pop16(); rCS = pop16(); uint16_t f = pop16();
			// Services report CF/ZF through the saved flags image like a real BIOS.
			f = uint16_t((f & ~0x0041) | (fCF ? 1 : 0) | (fZF ? 0x40 : 0));
			set_flags(f);
			lin = LIN(rCS, ip);
			continue;
		}
		if (rCS == kStubSeg) {
			rBP = pop16(); uint16_t ip2 = pop16(); rCS = pop16(); lin = LIN(rCS, ip2);
			continue;
		}
		// Prefer the function translated for this CS value: overlapping
		// mis-decodes from other segments must never own a real entry point.
		if (uint16_t f = fnByCs[rCS]) {
			uint32_t r = seg_table[f - 1].fn(lin);
			if (!(r & kNotHere)) { lin = r; continue; }
		}
		if (uint32_t idx = code_owner_lookup(lin)) {
			uint32_t r = seg_table[idx - 1].fn(lin);
			if (!(r & kNotHere)) { lin = r; continue; }
		}
		// Turbo Pascal's Intr(): "push bp; int n; pop bp; retf" built on the stack.
		if (mem[lin] == 0x55 && mem[lin + 1] == 0xCD && mem[lin + 3] == 0x5D && mem[lin + 4] == 0xCB) {
			int n = mem[lin + 2];
			push16(rBP);
			uint32_t t = do_int(n, kStubSeg, 0);
			if (t) { lin = t; continue; }
			rBP = pop16(); uint16_t ip = pop16(); rCS = pop16(); lin = LIN(rCS, ip);
			continue;
		}
		fatal("no code at %04x:%04x (lin %05x)", rCS, uint16_t(lin - (uint32_t(rCS) << 4)), lin);
	}
}

extern "C" uint32_t do_int(int n, uint16_t ret_cs, uint16_t ret_ip)
{
	uint16_t vo = rd16(n * 4), vs = rd16(n * 4 + 2);
	if (vs == kNativeSeg) {
		nativeInt(n);
		return 0;
	}
	push16(get_flags()); push16(ret_cs); push16(ret_ip);
	fIF = 0; fTF = 0;
	rCS = vs;
	return LIN(vs, vo);
}

void call_int_nested(int n)
{
	uint16_t vo = rd16(n * 4), vs = rd16(n * 4 + 2);
	if (vs == kNativeSeg) { nativeInt(n); return; }
	push16(get_flags()); push16(kReturnSeg); push16(0);
	fIF = 0; fTF = 0;
	uint16_t savedCS = rCS;
	rCS = vs;
	iretDepth++;
	run_loop(LIN(vs, vo));
	iretDepth--;
	rCS = savedCS;
}

extern "C" void irq_deliver(void)
{
	// Checked on every interrupt opportunity, so the game thread stops within
	// about a millisecond of a quit request instead of running on while the
	// process tears down.
	if (g.quitting) longjmp(exitJump, 3);
	if (!fIF) return;
	// The game's timer handler re-enables interrupts, so a slow machine could
	// otherwise nest handlers without bound and run the stack out. Real
	// hardware would hold the line until the interrupt controller is done.
	if (iretDepth >= 3) return;
	if (irq_pending & 1) {
		if (atomic_add(&timerPendingCount, -1) <= 1) atomic_and(IRQP, ~1);
		if (timerPendingCount < 0) atomic_set(&timerPendingCount, 0);
		++irq0Delivered;
		if (stats) {
			bigtime_t now = system_time();
			if (now - statsLast >= 5000000) {
				double el = (now - statsLast) / 1000000.0;
				logf("stats: IRQ0 %.0f/s raised, %.0f/s delivered, IRQ1 %.1f/s, frames %.1f/s",
					irq0Raised / el, irq0Delivered / el, irq1Delivered / el, double(vga_frame_count - framesLast) / el);
				framesLast = vga_frame_count;
				irq0Raised = irq0Delivered = irq1Delivered = 0; statsLast = now;
			}
		}
		call_int_nested(8);
		return;
	}
	if (irq_pending & 2) {
		{
			SpinLock l(&keyLock);
			if (keyHead == keyTail) { atomic_and(IRQP, ~2); return; }
			currentScancode = keyQueue[keyHead]; keyHead = (keyHead + 1) & 255;
			if (keyHead == keyTail) atomic_and(IRQP, ~2);
		}
		++irq1Delivered;
		call_int_nested(9);
		return;
	}
	atomic_set(IRQP, 0);
}

extern "C" void idle_spin(void)
{
	// Long run without a pending interrupt: the game is busy-waiting on a
	// tick or a retrace. Yield a little so the process does not burn a core.
	spin_count = 198000;
	if (g.quitting) longjmp(exitJump, 3);
	snooze(200);
}

void raise_irq(int line) { atomic_or(IRQP, 1 << line); }
double pit_channel_hz(int ch) { return pitHz(ch); }
bool speaker_enabled() { return (port61 & 3) == 3; }
uint32_t bios_ticks() { return rd16(0x46C) | (uint32_t(rd16(0x46E)) << 16); }

// ---- ports ----
extern "C" uint8_t port_in8(uint16_t port)
{
	uint8_t v = 0xff;
	if (vga_port_in(port, v)) return v;
	if (sound_port_in(port, v)) return v;
	switch (port) {
		case 0x60: return currentScancode;
		case 0x61: { static uint8_t toggle = 0; toggle ^= 0x10; return uint8_t((port61 & 0x0f) | toggle); }
		case 0x40: case 0x41: case 0x42: {
			int ch = port - 0x40; PitChannel& c = pit[ch];
			uint16_t val = c.latched ? c.latchValue : pitCurrentCount(ch);
			if (c.access == 1) { c.latched = false; return uint8_t(val); }
			if (c.access == 2) { c.latched = false; return uint8_t(val >> 8); }
			if (c.latchPhase == 0) { c.latchPhase = 1; if (!c.latched) { c.latchValue = val; c.latched = true; } return uint8_t(c.latchValue); }
			c.latchPhase = 0; c.latched = false; return uint8_t(c.latchValue >> 8);
		}
		case 0x20: return 0; case 0x21: return 0;
		case 0xA0: case 0xA1: return 0;
		case 0x201: return 0xf0;                       // joystick: no buttons, axes idle
		case 0x300: case 0x301: case 0x330: case 0x331: return 0xff;   // no MPU-401 / MT-32
		default: tracef("port in %03x", port); return 0xff;
	}
}

extern "C" void port_out8(uint16_t port, uint8_t v)
{
	if (vga_port_out(port, v)) return;
	if (sound_port_out(port, v)) return;
	switch (port) {
		case 0x43: {
			int ch = v >> 6; if (ch == 3) return;
			PitChannel& c = pit[ch];
			uint8_t access = (v >> 4) & 3;
			if (access == 0) { c.latchValue = pitCurrentCount(ch); c.latched = true; c.latchPhase = 0; return; }
			c.access = access; c.mode = (v >> 1) & 7; c.latchPhase = 0;
			return;
		}
		case 0x40: case 0x41: case 0x42: {
			int ch = port - 0x40; PitChannel& c = pit[ch];
			bool done = false;
			if (c.access == 1) { c.reload = v; done = true; }
			else if (c.access == 2) { c.reload = uint16_t(v << 8); done = true; }
			else if (c.latchPhase == 0) { c.reload = uint16_t((c.reload & 0xff00) | v); c.latchPhase = 1; }
			else { c.reload = uint16_t((c.reload & 0x00ff) | (v << 8)); c.latchPhase = 0; done = true; }
			if (done) { c.start = system_time(); if (ch == 0) tracef("pit: channel 0 reload %u (%.1f Hz)", c.reload, pitHz(0)); if (ch == 2) sound_speaker_update(); }
			return;
		}
		case 0x61: port61 = v; sound_speaker_update(); return;
		case 0x20: case 0x21: case 0xA0: case 0xA1: return;
		case 0x300: case 0x301: case 0x330: case 0x331: return;
		default: tracef("port out %03x = %02x", port, v); return;
	}
}

// ---- keyboard entry from the window thread ----
void input_key(uint32_t key, bool down)
{
	if (key >= 0x70 || kScan[key] == 0) return;
	uint16_t sc = kScan[key];
	bool ext = (sc & 0x100) != 0;
	pushBiosKey(sc & 0x7f, ext, down);
	updateBiosShiftFlags();
	SpinLock l(&keyLock);
	if (ext) pushKey(0xe0);
	pushKey(uint8_t((sc & 0x7f) | (down ? 0 : 0x80)));
	atomic_or(IRQP, 2);
}

// ---- CT-VOICE.DRV replacement: the game calls the driver entry with BX = function ----
extern "C" void ctvoice_call(void)
{
	switch (rBX) {
		case 0: rAX = 0x0202; break;                                  // driver version 2.02
		case 1: tracef("ctvoice: base port %03x", rAX); break;        // set base I/O address
		case 2: tracef("ctvoice: irq %u", rAX); break;                // set IRQ
		case 3: rAX = 0; break;                                       // initialise: success
		case 4: break;                                                // speaker on/off (AL)
		case 5: voc_set_status(LIN(rES, rDI)); break;                 // address of the status word
		case 6: voc_play(LIN(rES, rDI)); rAX = 0; break;              // output VOC data at ES:DI
		case 8: voc_stop(); break;                                    // stop output
		case 9: voc_stop(); break;                                    // terminate driver
		case 10: case 11: case 12: break;                             // pause / continue / break loop
		default: tracef("ctvoice: function %u", rBX); break;
	}
}

// ---- initialisation ----
namespace {
bool loadImage(std::string& error)
{
	FILE* f = fopen(g.exePath.c_str(), "rb");
	if (!f) { error = "cannot open " + g.exePath; return false; }
	std::vector<uint8_t> d; uint8_t buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
	fclose(f);
	if (d.size() < 28 || d[0] != 'M' || d[1] != 'Z') { error = "not an MZ executable"; return false; }
	uint16_t lastPage = d[2] | (d[3] << 8), pages = d[4] | (d[5] << 8), nreloc = d[6] | (d[7] << 8), hdr = uint16_t((d[8] | (d[9] << 8)) * 16);
	uint16_t ss = d[14] | (d[15] << 8), sp = d[16] | (d[17] << 8), ip = d[20] | (d[21] << 8), cs = d[22] | (d[23] << 8), reloff = d[24] | (d[25] << 8);
	size_t size = lastPage ? (pages - 1) * 512 + lastPage : pages * 512;
	if (size > d.size()) size = d.size();
	memset(mem, 0, sizeof mem);
	memcpy(mem + (kLoadSeg << 4), d.data() + hdr, size - hdr);
	for (int i = 0; i < nreloc; ++i) {
		uint16_t o = d[reloff + i * 4] | (d[reloff + i * 4 + 1] << 8), s = d[reloff + i * 4 + 2] | (d[reloff + i * 4 + 3] << 8);
		uint32_t a = (kLoadSeg << 4) + s * 16 + o;
		uint16_t v = uint16_t(mem[a] | (mem[a + 1] << 8)); v = uint16_t(v + kLoadSeg);
		mem[a] = uint8_t(v); mem[a + 1] = uint8_t(v >> 8);
	}
	rCS = uint16_t(cs + kLoadSeg); rSS = uint16_t(ss + kLoadSeg); rSP = sp;
	g.exitCode = 0;
	// PSP
	uint32_t psp = uint32_t(kPspSeg) << 4;
	mem[psp] = 0xCD; mem[psp + 1] = 0x20;
	wr16(psp + 2, kTopSeg);
	wr16(psp + 0x2C, kEnvSeg);
	{
		std::string tail = g.commandTail.substr(0, 126);
		mem[psp + 0x80] = uint8_t(tail.size());
		memcpy(mem + psp + 0x81, tail.data(), tail.size());
		mem[psp + 0x81 + tail.size()] = 0x0D;
	}
	// environment: "PATH=C:\" 0 "COMSPEC=C:\COMMAND.COM" 0 0, word 1, "C:\PREHISTO\HISTORIK.EXE" 0
	const char env[] = "PATH=C:\\\0COMSPEC=C:\\COMMAND.COM\0\0\1\0C:\\PREHISTO\\HISTORIK.EXE\0";
	memcpy(mem + (uint32_t(kEnvSeg) << 4), env, sizeof env);
	// IVT: everything native
	for (int i = 0; i < 256; ++i) { wr16(i * 4, uint16_t(i)); wr16(i * 4 + 2, kNativeSeg); }
	// BIOS data area
	wr16(0x410, 0x0021 | 0x4000);   // equipment: 80x25 colour, 1 floppy... 0x4000 = game port present? keep simple
	wr16(0x413, 640);
	mem[0x449] = 3; wr16(0x44A, 80); wr16(0x44C, 4000); wr16(0x44E, 0); wr16(0x450, 0);
	wr16(0x460, 0x0607); mem[0x462] = 0; wr16(0x463, 0x3D4);
	mem[0x484] = 24; wr16(0x485, 16); mem[0x487] = 0x60; mem[0x488] = 0x09; mem[0x489] = 0x11;
	wr16(0x46C, 0); wr16(0x46E, 0); mem[0x470] = 0;
	wr16(0x41A, 0x1E); wr16(0x41C, 0x1E); wr16(0x480, 0x1E); wr16(0x482, 0x3E);
	rDS = kPspSeg; rES = kPspSeg;
	rAX = 0; rBX = 0; rCX = 0; rDX = 0; rSI = 0; rDI = 0; rBP = 0;
	fIF = 1; fDF = 0;
	return true;
}
}

bool machineInit(std::string& error)
{
	if (!loadImage(error)) return false;
	fnIndex.assign(IMG_SIZE, 0);
	for (int i = 0; i < code_range_count; ++i)
		for (uint32_t a = code_ranges[i].start; a < code_ranges[i].end; ++a)
			if (a >= IMG_BASE && a - IMG_BASE < IMG_SIZE) fnIndex[a - IMG_BASE] = uint16_t(code_ranges[i].fn + 1);
	if (getenv("PREH_STATS")) { stats = true; statsLast = system_time(); }
	for (int i = 0; i < seg_count; ++i) fnByCs[seg_table[i].cs] = uint16_t(i + 1);
	for (int i = 0; i < 5; ++i) { files[i].open = true; files[i].isDevice = true; }
	for (int i = 0; i < 3; ++i) pit[i].start = system_time();
	pit[0].reload = 0;
	vga_init();
	sound_init();
	return true;
}

thread_id pit_thread() { return timerThread; }

void pit_tick_thread_start()
{
	timerThread = spawn_thread(timerLoop, "pit", B_URGENT_DISPLAY_PRIORITY, NULL);
	resume_thread(timerThread);
}

void gameMain()
{
	int r = setjmp(exitJump);
	if (r == 0) {
		pit_tick_thread_start();
		uint32_t entry = LIN(rCS, 0);
		logf("prehistorik: starting at %04x:0000, stack %04x:%04x", rCS, rSS, rSP);
		run_loop(entry);
		logf("prehistorik: program returned to dispatcher");
	} else if (r == 1) {
		logf("prehistorik: program exited with code %d", g.exitCode);
	} else if (r == 3) {
		logf("prehistorik: quit requested");
	} else {
		logf("prehistorik: aborted");
	}
	g.quitting = true;
	sound_shutdown();
}
