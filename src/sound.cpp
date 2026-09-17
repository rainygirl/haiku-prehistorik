// Native sound: OPL2 (AdLib at 388h) through ymfm, Creative VOC playback for
// the CT-VOICE driver replacement, and the PC speaker (PIT channel 2 /
// port 61h). Everything is mixed into one BSoundPlayer stream.
#include "machine.h"
#include "ymfm/ymfm_opl.h"
#ifdef __HAIKU__
#include <SoundPlayer.h>
#endif
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

const uint32_t kOplClock = 3579545;
int32 chipLock = 0;
struct Lock { Lock() { while (atomic_test_and_set(&chipLock, 1, 0) != 0) snooze(20); } ~Lock() { atomic_set(&chipLock, 0); } };

class OplInterface : public ymfm::ymfm_interface {
public:
	int64_t timerExpiry[2];
	int64_t clocks = 0;
	OplInterface() { timerExpiry[0] = timerExpiry[1] = -1; }
	void ymfm_set_timer(uint32_t tnum, int32_t duration) override { timerExpiry[tnum] = duration < 0 ? -1 : clocks + duration; }
	void expire() { for (int t = 0; t < 2; ++t) if (timerExpiry[t] >= 0 && clocks >= timerExpiry[t]) { timerExpiry[t] = -1; m_engine->engine_timer_expired(t); } }
};
OplInterface oplInterface;
ymfm::ym3812* opl = NULL;
uint32_t rate = 49716;
bigtime_t oplTimeBase = 0;

void oplAdvanceToWallClock()
{
	bigtime_t now = system_time();
	int64_t wanted = int64_t((now - oplTimeBase) * (double(kOplClock) / 1000000.0));
	if (wanted > oplInterface.clocks) oplInterface.clocks = wanted;
	oplInterface.expire();
}

// ---- VOC ----
struct Voc {
	bool playing = false;
	std::vector<uint8_t> data;   // decoded 8-bit unsigned samples at `sampleRate`
	double sampleRate = 11025;
	double pos = 0;
	uint32_t statusLin = 0;
	int repeatStart = -1, repeatCount = 0;
} voc;

void vocFinish()
{
	voc.playing = false;
	if (voc.statusLin) { mem[voc.statusLin] = 0; mem[voc.statusLin + 1] = 0; }
}

bool sbInReset = false, sbReady = false;

// ---- speaker ----
double speakerPhase = 0;
bool speakerOn = false;
double speakerHz = 0;

// ---- output ----
bool muted = false;
FILE* pcmDump = NULL;

void mix(int16_t* out, int frames)
{
	Lock lock;
	std::vector<ymfm::ym3812::output_data> fm(frames);
	opl->generate(fm.data(), frames);
	oplInterface.clocks += int64_t(frames) * 72;   // ym3812: clock / 72 per sample
	oplInterface.expire();
	double step = voc.sampleRate / rate;
	double spStep = speakerHz / rate;
	for (int i = 0; i < frames; ++i) {
		int32_t s = fm[i].data[0];
		if (voc.playing) {
			size_t p = size_t(voc.pos);
			if (p + 1 < voc.data.size()) {
				double frac = voc.pos - p;
				double v = voc.data[p] * (1 - frac) + voc.data[p + 1] * frac;
				s += int32_t((v - 128) * 120);
				voc.pos += step;
			} else vocFinish();
		}
		if (speakerOn && speakerHz > 20 && speakerHz < 20000) {
			speakerPhase += spStep; if (speakerPhase >= 1) speakerPhase -= 1;
			s += speakerPhase < 0.5 ? 6000 : -6000;
		}
		if (s > 32767) s = 32767; if (s < -32768) s = -32768;
		out[i * 2] = out[i * 2 + 1] = int16_t(s);
	}
	if (pcmDump) fwrite(out, 4, frames, pcmDump);
	if (muted) memset(out, 0, size_t(frames) * 4);
}

#ifdef __HAIKU__
BSoundPlayer* player = NULL;
void fillBuffer(void*, void* buffer, size_t size, const media_raw_audio_format& format)
{
	mix(static_cast<int16_t*>(buffer), int(size / (format.channel_count * sizeof(int16_t))));
}
#else
volatile bool hostAudioRunning = false;
int32 hostAudioLoop(void*)
{
	// Headless builds: pull audio at real-time pace so timing matches, output only to the PCM dump.
	int16_t buf[512 * 2];
	bigtime_t next = system_time();
	while (hostAudioRunning && !g.quitting) {
		mix(buf, 512);
		next += bigtime_t(512 * 1000000.0 / rate);
		bigtime_t now = system_time(); if (next > now) snooze(next - now);
	}
	return 0;
}
#endif

} // namespace

void sound_init()
{
	opl = new ymfm::ym3812(oplInterface);
	opl->reset();
	rate = opl->sample_rate(kOplClock);
	oplTimeBase = system_time();
	if (const char* f = getenv("PREH_PCM")) pcmDump = fopen(f, "wb");
	// PREH_NOSOUND=1 keeps everything running (and the PCM dump) but sends silence to the device.
	if (getenv("PREH_NOSOUND")) muted = true;
	if (g.noAudio) return;
#ifndef __HAIKU__
	hostAudioRunning = true; resume_thread(spawn_thread(hostAudioLoop, "audio", B_NORMAL_PRIORITY, NULL));
	return;
#else
	media_raw_audio_format fmt = media_raw_audio_format::wildcard;
	fmt.frame_rate = float(rate); fmt.channel_count = 2; fmt.format = media_raw_audio_format::B_AUDIO_SHORT;
	fmt.byte_order = B_MEDIA_HOST_ENDIAN; fmt.buffer_size = 1024 * 4;
	player = new BSoundPlayer(&fmt, "Prehistorik", fillBuffer);
	if (player->InitCheck() != B_OK) { logf("sound: BSoundPlayer failed (%s)", strerror(player->InitCheck())); delete player; player = NULL; return; }
	player->Start(); player->SetHasData(true);
	logf("sound: OPL2 at %u Hz", rate);
#endif
}

void sound_shutdown()
{
	// Called from the application thread on quit and from the game thread when
	// the program ends; only the first call does the work.
	static int32 done = 0;
	if (atomic_test_and_set(&done, 1, 0) != 0) return;
#ifdef __HAIKU__
	// Stop synchronously and wait for the media node to be idle: deleting the
	// player while its control thread is still calling back crashes in the
	// media kit.
	if (player) {
		BSoundPlayer* p = player;
		player = NULL;
		p->SetHasData(false);
		p->Stop(true, true);
		snooze(50000);
		delete p;
	}
#else
	hostAudioRunning = false;
	snooze(50000);
#endif
	if (pcmDump) { fclose(pcmDump); pcmDump = NULL; }
}

bool sound_port_in(uint16_t port, uint8_t& v)
{
	if (port == 0x388) {
		// Each status read on a real ISA bus costs about a microsecond; detection
		// loops count on that to let the 80 us OPL timer expire.
		Lock l; oplInterface.clocks += 4; oplAdvanceToWallClock(); v = opl->read_status(); return true;
	}
	if (port == 0x389) { v = 0xff; return true; }
	// Sound Blaster DSP probe at 220h: answer the reset handshake so the game
	// believes a card is present; the digital output itself is native (VOC).
	if (port == 0x22A) { v = sbReady ? 0xAA : 0xff; sbReady = false; return true; }
	if (port == 0x22E) { v = sbReady ? 0xff : 0x7f; return true; }
	if (port == 0x22C) { v = 0x7f; return true; }
	if (port >= 0x220 && port < 0x230) { v = 0xff; return true; }
	return false;
}

bool sound_port_out(uint16_t port, uint8_t v)
{
	if (port == 0x388) { Lock l; opl->write_address(v); return true; }
	if (port == 0x389) { Lock l; opl->write_data(v); return true; }
	if (port == 0x226) { if (v & 1) sbInReset = true; else if (sbInReset) { sbInReset = false; sbReady = true; } return true; }
	if (port >= 0x220 && port < 0x230) return true;
	return false;
}

void sound_speaker_update()
{
	Lock l;
	speakerOn = speaker_enabled();
	speakerHz = pit_channel_hz(2);
}

// CT-VOICE function 5: pointer to the status word (0xFFFF while playing, 0 when done).
void voc_set_status(uint32_t lin) { Lock l; voc.statusLin = lin; }

bool voc_playing() { return voc.playing; }

void voc_stop() { Lock l; vocFinish(); }

// CT-VOICE function 6: parse the VOC blocks at lin and start playback.
void voc_play(uint32_t lin)
{
	Lock l;
	std::vector<uint8_t> samples; double sr = 11025; bool haveRate = false;
	uint32_t p = lin;
	if (memcmp(mem + lin, "Creative Voice File", 19) == 0) p = lin + (mem[lin + 20] | (mem[lin + 21] << 8));
	int guard = 0;
	while (p < MEM_SIZE - 4 && guard++ < 256) {
		uint8_t type = mem[p];
		if (type == 0) break;
		uint32_t len = mem[p + 1] | (mem[p + 2] << 8) | (uint32_t(mem[p + 3]) << 16);
		uint32_t body = p + 4;
		if (body + len > MEM_SIZE) break;
		if (type == 1) {
			uint8_t tc = mem[body];
			if (!haveRate) { sr = 1000000.0 / (256 - tc); haveRate = true; }
			if (mem[body + 1] == 0 && len >= 2) samples.insert(samples.end(), mem + body + 2, mem + body + len);
		} else if (type == 2) {
			samples.insert(samples.end(), mem + body, mem + body + len);
		} else if (type == 3) {
			uint16_t n = mem[body] | (mem[body + 1] << 8); uint8_t tc = mem[body + 2];
			double r = 1000000.0 / (256 - tc); size_t cnt = size_t(n * (haveRate ? sr / r : 1));
			samples.insert(samples.end(), cnt, 0x80);
		} else if (type == 9) {
			uint32_t r = mem[body] | (mem[body + 1] << 8) | (uint32_t(mem[body + 2]) << 16) | (uint32_t(mem[body + 3]) << 24);
			uint8_t bits = mem[body + 4], ch = mem[body + 5];
			if (!haveRate) { sr = r; haveRate = true; }
			if (bits == 8 && ch == 1) samples.insert(samples.end(), mem + body + 12, mem + body + len);
		}
		p = body + len;
	}
	voc.data.swap(samples);
	voc.sampleRate = sr; voc.pos = 0;
	voc.playing = !voc.data.empty();
	if (voc.statusLin) { mem[voc.statusLin] = voc.playing ? 0xff : 0; mem[voc.statusLin + 1] = voc.playing ? 0xff : 0; }
	tracef("voc: play %u samples at %.0f Hz", unsigned(voc.data.size()), sr);
}
