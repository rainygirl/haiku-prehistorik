/* Machine model shared by the translated game code (C) and the native
 * runtime (C++). The 8086 registers and flags are globals; memory is a flat
 * 1 MB array with the VGA window (A0000-BFFFF) routed through the video
 * emulation. Every arithmetic helper computes the real 8086 flags. */
#ifndef PREHISTORIK_CPU_H
#define PREHISTORIK_CPU_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_BASE 0x1000u          /* load segment 0100h */
#define IMG_SIZE 0x28000u         /* covers the whole MZ image */
#define MEM_SIZE 0x110000u

extern uint16_t rAX, rBX, rCX, rDX, rSI, rDI, rBP, rSP, rCS, rDS, rES, rSS;
extern uint8_t fCF, fPF, fAF, fZF, fSF, fTF, fIF, fDF, fOF;
extern uint8_t mem[MEM_SIZE];
extern volatile int32_t irq_pending;   /* bit mask of pending IRQ lines */
extern uint32_t spin_count;

#define AL (*(uint8_t*)&rAX)
#define AH (*((uint8_t*)&rAX + 1))
#define BL (*(uint8_t*)&rBX)
#define BH (*((uint8_t*)&rBX + 1))
#define CL (*(uint8_t*)&rCX)
#define CH (*((uint8_t*)&rCX + 1))
#define DL (*(uint8_t*)&rDX)
#define DH (*((uint8_t*)&rDX + 1))

#define LIN(seg, off) ((((uint32_t)(uint16_t)(seg)) << 4) + (uint16_t)(off))

/* ---- native services (runtime.cpp / vga.cpp / sound.cpp) ---- */
uint8_t vga_read8(uint32_t a);
void vga_write8(uint32_t a, uint8_t v);
uint8_t port_in8(uint16_t port);
void port_out8(uint16_t port, uint8_t v);
uint32_t do_int(int n, uint16_t ret_cs, uint16_t ret_ip);   /* 0 = handled natively */
void irq_deliver(void);
void idle_spin(void);
void fatal(const char* fmt, ...);
void trace_ins(uint32_t lin);
void ctvoice_call(void);
void div_zero(void);

/* ---- memory ---- */
static inline uint8_t rd8(uint32_t a)
{
	if ((a - 0xA0000u) < 0x20000u) return vga_read8(a);
	return mem[a];
}
static inline void wr8(uint32_t a, uint8_t v)
{
	if ((a - 0xA0000u) < 0x20000u) { vga_write8(a, v); return; }
	mem[a] = v;
}
static inline uint16_t rd16(uint32_t a)
{
	if ((a - 0xA0000u) < 0x20000u) return (uint16_t)(vga_read8(a) | (vga_read8(a + 1) << 8));
	return (uint16_t)(mem[a] | (mem[a + 1] << 8));
}
static inline void wr16(uint32_t a, uint16_t v)
{
	if ((a - 0xA0000u) < 0x20000u) { vga_write8(a, (uint8_t)v); vga_write8(a + 1, (uint8_t)(v >> 8)); return; }
	mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8);
}
static inline void push16(uint16_t v) { rSP = (uint16_t)(rSP - 2); wr16(LIN(rSS, rSP), v); }
static inline uint16_t pop16(void) { uint16_t v = rd16(LIN(rSS, rSP)); rSP = (uint16_t)(rSP + 2); return v; }

/* ---- flags ---- */
extern const uint8_t parity_table[256];
static inline void szp8(uint8_t r) { fZF = r == 0; fSF = r >> 7; fPF = parity_table[r]; }
static inline void szp16(uint16_t r) { fZF = r == 0; fSF = r >> 15; fPF = parity_table[r & 0xff]; }

static inline uint16_t get_flags(void)
{
	return (uint16_t)(0xF002u | fCF | (fPF << 2) | (fAF << 4) | (fZF << 6) | (fSF << 7) | (fTF << 8) | (fIF << 9) | (fDF << 10) | (fOF << 11));
}
static inline void set_flags(uint16_t f)
{
	fCF = f & 1; fPF = (f >> 2) & 1; fAF = (f >> 4) & 1; fZF = (f >> 6) & 1; fSF = (f >> 7) & 1;
	fTF = (f >> 8) & 1; fIF = (f >> 9) & 1; fDF = (f >> 10) & 1; fOF = (f >> 11) & 1;
}

static inline uint8_t add8(uint8_t a, uint8_t b) { uint16_t r = (uint16_t)(a + b); fCF = r >> 8; fOF = ((a ^ r) & (b ^ r) & 0x80) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t add16(uint16_t a, uint16_t b) { uint32_t r = (uint32_t)a + b; fCF = r >> 16; fOF = ((a ^ r) & (b ^ r) & 0x8000) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t adc8(uint8_t a, uint8_t b) { uint16_t r = (uint16_t)(a + b + fCF); fCF = r >> 8; fOF = ((a ^ r) & (b ^ r) & 0x80) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t adc16(uint16_t a, uint16_t b) { uint32_t r = (uint32_t)a + b + fCF; fCF = r >> 16; fOF = ((a ^ r) & (b ^ r) & 0x8000) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t sub8(uint8_t a, uint8_t b) { uint16_t r = (uint16_t)(a - b); fCF = (r >> 8) & 1; fOF = ((a ^ b) & (a ^ r) & 0x80) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t sub16(uint16_t a, uint16_t b) { uint32_t r = (uint32_t)a - b; fCF = (r >> 16) & 1; fOF = ((a ^ b) & (a ^ r) & 0x8000) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t sbb8(uint8_t a, uint8_t b) { uint16_t r = (uint16_t)(a - b - fCF); fCF = (r >> 8) & 1; fOF = ((a ^ b) & (a ^ r) & 0x80) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp8((uint8_t)r); return (uint8_t)r; }
static inline uint16_t sbb16(uint16_t a, uint16_t b) { uint32_t r = (uint32_t)a - b - fCF; fCF = (r >> 16) & 1; fOF = ((a ^ b) & (a ^ r) & 0x8000) != 0; fAF = ((a ^ b ^ r) & 0x10) != 0; szp16((uint16_t)r); return (uint16_t)r; }
static inline uint8_t inc8(uint8_t a) { uint8_t r = (uint8_t)(a + 1); fOF = r == 0x80; fAF = (r & 0xf) == 0; szp8(r); return r; }
static inline uint16_t inc16(uint16_t a) { uint16_t r = (uint16_t)(a + 1); fOF = r == 0x8000; fAF = (r & 0xf) == 0; szp16(r); return r; }
static inline uint8_t dec8(uint8_t a) { uint8_t r = (uint8_t)(a - 1); fOF = r == 0x7f; fAF = (a & 0xf) == 0; szp8(r); return r; }
static inline uint16_t dec16(uint16_t a) { uint16_t r = (uint16_t)(a - 1); fOF = r == 0x7fff; fAF = (a & 0xf) == 0; szp16(r); return r; }
static inline uint8_t and8(uint8_t a, uint8_t b) { uint8_t r = a & b; fCF = fOF = fAF = 0; szp8(r); return r; }
static inline uint16_t and16(uint16_t a, uint16_t b) { uint16_t r = a & b; fCF = fOF = fAF = 0; szp16(r); return r; }
static inline uint8_t or8(uint8_t a, uint8_t b) { uint8_t r = a | b; fCF = fOF = fAF = 0; szp8(r); return r; }
static inline uint16_t or16(uint16_t a, uint16_t b) { uint16_t r = a | b; fCF = fOF = fAF = 0; szp16(r); return r; }
static inline uint8_t xor8(uint8_t a, uint8_t b) { uint8_t r = a ^ b; fCF = fOF = fAF = 0; szp8(r); return r; }
static inline uint16_t xor16(uint16_t a, uint16_t b) { uint16_t r = a ^ b; fCF = fOF = fAF = 0; szp16(r); return r; }
static inline uint8_t neg8(uint8_t a) { uint8_t r = (uint8_t)(0 - a); fCF = a != 0; fOF = a == 0x80; fAF = (a & 0xf) != 0; szp8(r); return r; }
static inline uint16_t neg16(uint16_t a) { uint16_t r = (uint16_t)(0 - a); fCF = a != 0; fOF = a == 0x8000; fAF = (a & 0xf) != 0; szp16(r); return r; }

static inline uint8_t shl8(uint8_t a, uint8_t n) { n &= 31; if (!n) return a; if (n > 8) { fCF = 0; a = 0; } else { fCF = (a >> (8 - n)) & 1; a = (uint8_t)(a << n); } fOF = ((a >> 7) ^ fCF) & 1; szp8(a); return a; }
static inline uint16_t shl16(uint16_t a, uint8_t n) { n &= 31; if (!n) return a; if (n > 16) { fCF = 0; a = 0; } else { fCF = (a >> (16 - n)) & 1; a = (uint16_t)(a << n); } fOF = ((a >> 15) ^ fCF) & 1; szp16(a); return a; }
static inline uint8_t shr8(uint8_t a, uint8_t n) { n &= 31; if (!n) return a; fOF = a >> 7; if (n > 8) { fCF = 0; a = 0; } else { fCF = (a >> (n - 1)) & 1; a = (uint8_t)(a >> n); } szp8(a); return a; }
static inline uint16_t shr16(uint16_t a, uint8_t n) { n &= 31; if (!n) return a; fOF = a >> 15; if (n > 16) { fCF = 0; a = 0; } else { fCF = (a >> (n - 1)) & 1; a = (uint16_t)(a >> n); } szp16(a); return a; }
static inline uint8_t sar8(uint8_t a, uint8_t n) { n &= 31; if (!n) return a; int8_t s = (int8_t)a; if (n > 8) n = 8; fCF = (s >> (n - 1)) & 1; s = (int8_t)(s >> n); fOF = 0; szp8((uint8_t)s); return (uint8_t)s; }
static inline uint16_t sar16(uint16_t a, uint8_t n) { n &= 31; if (!n) return a; int16_t s = (int16_t)a; if (n > 16) n = 16; fCF = (s >> (n - 1)) & 1; s = (int16_t)(s >> n); fOF = 0; szp16((uint16_t)s); return (uint16_t)s; }
static inline uint8_t rol8(uint8_t a, uint8_t n) { n &= 31; if (!n) return a; n &= 7; a = (uint8_t)((a << n) | (a >> ((8 - n) & 7))); fCF = a & 1; fOF = ((a >> 7) ^ fCF) & 1; return a; }
static inline uint16_t rol16(uint16_t a, uint8_t n) { n &= 31; if (!n) return a; n &= 15; a = (uint16_t)((a << n) | (a >> ((16 - n) & 15))); fCF = a & 1; fOF = ((a >> 15) ^ fCF) & 1; return a; }
static inline uint8_t ror8(uint8_t a, uint8_t n) { n &= 31; if (!n) return a; n &= 7; a = (uint8_t)((a >> n) | (a << ((8 - n) & 7))); fCF = a >> 7; fOF = ((a >> 7) ^ (a >> 6)) & 1; return a; }
static inline uint16_t ror16(uint16_t a, uint8_t n) { n &= 31; if (!n) return a; n &= 15; a = (uint16_t)((a >> n) | (a << ((16 - n) & 15))); fCF = a >> 15; fOF = ((a >> 15) ^ (a >> 14)) & 1; return a; }
static inline uint8_t rcl8(uint8_t a, uint8_t n) { n = (n & 31) % 9; if (!n) return a; uint16_t v = (uint16_t)(a | (fCF << 8)); v = (uint16_t)((v << n) | (v >> (9 - n))); fCF = (v >> 8) & 1; a = (uint8_t)v; fOF = ((a >> 7) ^ fCF) & 1; return a; }
static inline uint16_t rcl16(uint16_t a, uint8_t n) { n = (n & 31) % 17; if (!n) return a; uint32_t v = a | ((uint32_t)fCF << 16); v = (v << n) | (v >> (17 - n)); fCF = (v >> 16) & 1; a = (uint16_t)v; fOF = ((a >> 15) ^ fCF) & 1; return a; }
static inline uint8_t rcr8(uint8_t a, uint8_t n) { n = (n & 31) % 9; if (!n) return a; uint16_t v = (uint16_t)(a | (fCF << 8)); v = (uint16_t)((v >> n) | (v << (9 - n))); fCF = (v >> 8) & 1; a = (uint8_t)v; fOF = ((a >> 7) ^ (a >> 6)) & 1; return a; }
static inline uint16_t rcr16(uint16_t a, uint8_t n) { n = (n & 31) % 17; if (!n) return a; uint32_t v = a | ((uint32_t)fCF << 16); v = (v >> n) | (v << (17 - n)); fCF = (v >> 16) & 1; a = (uint16_t)v; fOF = ((a >> 15) ^ (a >> 14)) & 1; return a; }

static inline void mul8(uint8_t b) { uint16_t r = (uint16_t)AL * b; rAX = r; fCF = fOF = (r >> 8) != 0; }
static inline void mul16(uint16_t b) { uint32_t r = (uint32_t)rAX * b; rAX = (uint16_t)r; rDX = (uint16_t)(r >> 16); fCF = fOF = rDX != 0; }
static inline void imul8(uint8_t b) { int16_t r = (int16_t)(int8_t)AL * (int8_t)b; rAX = (uint16_t)r; fCF = fOF = r != (int8_t)r; }
static inline void imul16(uint16_t b) { int32_t r = (int32_t)(int16_t)rAX * (int16_t)b; rAX = (uint16_t)r; rDX = (uint16_t)((uint32_t)r >> 16); fCF = fOF = r != (int16_t)r; }
static inline void div8(uint8_t b) { if (!b) { div_zero(); return; } uint16_t n = rAX; uint16_t q = n / b; if (q > 0xff) { div_zero(); return; } AL = (uint8_t)q; AH = (uint8_t)(n % b); }
static inline void div16(uint16_t b) { if (!b) { div_zero(); return; } uint32_t n = ((uint32_t)rDX << 16) | rAX; uint32_t q = n / b; if (q > 0xffff) { div_zero(); return; } rAX = (uint16_t)q; rDX = (uint16_t)(n % b); }
static inline void idiv8(uint8_t b) { if (!b) { div_zero(); return; } int16_t n = (int16_t)rAX; int16_t q = n / (int8_t)b; if (q != (int8_t)q) { div_zero(); return; } AL = (uint8_t)q; AH = (uint8_t)(n % (int8_t)b); }
static inline void idiv16(uint16_t b) { if (!b) { div_zero(); return; } int32_t n = (int32_t)(((uint32_t)rDX << 16) | rAX); int32_t q = n / (int16_t)b; if (q != (int16_t)q) { div_zero(); return; } rAX = (uint16_t)q; rDX = (uint16_t)(n % (int16_t)b); }

/* ---- string helpers: dir = +1/-1 already applied by caller via fDF ---- */
#define SDIR(n) (fDF ? (uint16_t)(0 - (n)) : (uint16_t)(n))

/* ---- pending interrupt check at loop back-edges and calls ---- */
static inline void irq_check(void)
{
	if (irq_pending) { irq_deliver(); spin_count = 0; }
	else if (++spin_count >= 200000u) idle_spin();
}

#ifndef TRACE
#define TRACE(lin) ((void)0)
#else
#define TRACE(lin) trace_ins(lin)
#endif

#ifdef __cplusplus
}
#endif
#endif
