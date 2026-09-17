// Headless test driver (macOS/Linux): runs the translated game, injects Haiku
// key codes on a schedule and writes PNG snapshots of the emulated screen.
// Usage: preh-host GAME_EXE SECONDS [shot_ms[,shot_ms...]] [key:ms,...]
#include "machine.h"
#include <zlib.h>
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

struct ThreadStart { thread_func f; void* arg; };
static void* threadTrampoline(void* p) { ThreadStart* s = static_cast<ThreadStart*>(p); s->f(s->arg); delete s; return NULL; }
thread_id spawn_thread(thread_func f, const char*, int32, void* arg)
{
	pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
	pthread_create(&t, &a, threadTrampoline, new ThreadStart{ f, arg }); pthread_detach(t); return 1;
}

static void writePng(const char* path, const uint32_t* px, int w, int h)
{
	std::vector<uint8_t> raw; raw.reserve((w * 3 + 1) * h);
	for (int y = 0; y < h; ++y) { raw.push_back(0); for (int x = 0; x < w; ++x) { uint32_t c = px[y * w + x]; raw.push_back(uint8_t(c >> 16)); raw.push_back(uint8_t(c >> 8)); raw.push_back(uint8_t(c)); } }
	uLongf clen = compressBound(raw.size()); std::vector<uint8_t> comp(clen);
	compress2(comp.data(), &clen, raw.data(), raw.size(), 6); comp.resize(clen);
	FILE* f = fopen(path, "wb"); if (!f) return;
	auto be32 = [&](uint32_t v) { uint8_t b[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) }; fwrite(b, 1, 4, f); };
	auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
		be32(uint32_t(data.size())); fwrite(type, 1, 4, f); if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
		uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4); if (!data.empty()) crc = crc32(crc, data.data(), uInt(data.size())); be32(uint32_t(crc)); };
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	std::vector<uint8_t> ihdr = { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr); chunk("IDAT", comp); chunk("IEND", {});
	fclose(f);
}

static int32 gameThread(void*) { gameMain(); return 0; }

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s historik.exe seconds [shots_ms] [key:ms,...] [outdir]\n", argv[0]); return 1; }
	g.exePath = argv[1];
	size_t s = g.exePath.find_last_of('/'); g.gameDir = s == std::string::npos ? "." : g.exePath.substr(0, s);
	double secs = atof(argv[2]);
	std::vector<unsigned> shots; if (argc > 3) { std::string l = argv[3]; size_t p = 0; while (p < l.size()) { size_t q = l.find(',', p); if (q == std::string::npos) q = l.size(); if (q > p) shots.push_back(unsigned(atoi(l.substr(p, q - p).c_str()))); p = q + 1; } }
	struct Key { unsigned code, ms; bool down; }; std::vector<Key> keys;
	if (argc > 4) {
		std::string l = argv[4]; size_t p = 0;
		while (p < l.size()) {
			size_t q = l.find(',', p); if (q == std::string::npos) q = l.size();
			std::string it = l.substr(p, q - p); p = q + 1; size_t c = it.find(':'); if (c == std::string::npos) continue;
			// "code:ms" = tap (100 ms); "code:ms:hold" = hold for hold ms
			unsigned code = unsigned(strtoul(it.c_str(), NULL, 16)); unsigned ms = unsigned(atoi(it.c_str() + c + 1)); unsigned hold = 100;
			size_t c2 = it.find(':', c + 1); if (c2 != std::string::npos) hold = unsigned(atoi(it.c_str() + c2 + 1));
			keys.push_back({ code, ms, true }); keys.push_back({ code, ms + hold, false });
		}
	}
	std::string outdir = argc > 5 ? argv[5] : "build/host";
	if (const char* t = getenv("PREH_TAIL")) g.commandTail = t;
	if (getenv("PREH_TRACE")) { g.trace = true; if (const char* f = getenv("PREH_TRACE_FILE")) { g.traceFile = fopen(f, "w"); } }
	std::string err;
	if (!machineInit(err)) { fprintf(stderr, "init failed: %s\n", err.c_str()); return 1; }
	spawn_thread(gameThread, "game", 0, NULL);
	bigtime_t start = system_time();
	std::vector<uint32_t> px(640 * 480), last(640 * 480); int w = 0, h = 0, lw = 0, lh = 0;
	size_t nextShot = 0;
	std::vector<bool> keyDone(keys.size(), false);
	while (!g.quitting) {
		unsigned el = unsigned((system_time() - start) / 1000);
		for (size_t i = 0; i < keys.size(); ++i) if (!keyDone[i] && el >= keys[i].ms) { input_key(keys[i].code, keys[i].down); keyDone[i] = true; }
		if (vga_compose(px.data(), w, h)) { last = px; lw = w; lh = h; }
		if (nextShot < shots.size() && el >= shots[nextShot]) {
			char path[512]; snprintf(path, sizeof path, "%s/shot_%06u.png", outdir.c_str(), shots[nextShot]);
			if (lw) { writePng(path, last.data(), lw, lh); fprintf(stderr, "[host] %s (%dx%d) mode %02x\n", path, lw, lh, vga_get_mode()); }
			nextShot++;
		}
		if (el >= unsigned(secs * 1000)) break;
		snooze(1000000 / 60);
	}
	fprintf(stderr, "[host] done, quitting=%d\n", int(g.quitting));
	if (g.traceFile) fflush(g.traceFile);
	g.quitting = true;
	snooze(100000);
	sound_shutdown();
	_exit(0);
}
