// Native runtime interfaces shared by runtime.cpp, vga.cpp, sound.cpp, app.cpp.
#ifndef PREHISTORIK_MACHINE_H
#define PREHISTORIK_MACHINE_H
#include "cpu.h"
#include <OS.h>
#include <string>
#include <cstdio>

struct Globals {
	std::string exePath, gameDir;
	volatile bool quitting = false;
	bool trace = false;
	FILE* traceFile = NULL;
	int exitCode = 0;
	std::string commandTail;      // DOS command line after the program name, e.g. " R"
	bool noAudio = false;         // do not open the audio device (headless tools)
	std::string configPath;       // if set: grawaga.cfg is read from here, writes go to a scratch file
};
extern Globals g;

void logf(const char* fmt, ...);
void tracef(const char* fmt, ...);

// runtime.cpp
bool machineInit(std::string& error);
void gameMain();                 // runs the translated program on the calling thread
void run_loop(uint32_t lin);     // dispatcher (re-entrant)
void call_int_nested(int n);
void raise_irq(int line);
uint32_t bios_ticks();
void input_key(uint32_t haikuKey, bool down);
void pit_tick_thread_start();
thread_id pit_thread();
double pit_channel_hz(int ch);
bool speaker_enabled();
uint32_t code_owner_lookup(uint32_t lin);

// vga.cpp
void vga_init();
bool vga_port_out(uint16_t port, uint8_t v);
bool vga_port_in(uint16_t port, uint8_t& v);
void vga_set_mode(uint8_t mode);
uint8_t vga_get_mode();
bool vga_compose(uint32_t* out, int& w, int& h);   // returns false when nothing changed
extern uint64_t vga_frame_count;
void vga_bios_int10();
void vga_text_scroll(int dir, uint8_t attr, int top, int left, int bottom, int right, int lines);

// sound.cpp
void sound_init();
void sound_shutdown();
bool sound_port_out(uint16_t port, uint8_t v);
bool sound_port_in(uint16_t port, uint8_t& v);
void sound_speaker_update();
void voc_play(uint32_t lin);      // CT-VOICE function 6: ES:DI -> VOC data
void voc_stop();
void voc_set_status(uint32_t lin);
bool voc_playing();

#endif
