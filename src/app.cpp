// Haiku front end: a window showing the emulated VGA frame, keyboard
// forwarding, and the thread running the translated game code.
#include "machine.h"
#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Invoker.h>
#include <Entry.h>
#include <Path.h>
#include <Roster.h>
#include <View.h>
#include <Window.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/stat.h>

namespace {

thread_id gameThread = -1, displayThread = -1;

class GameView : public BView {
public:
	GameView(BRect frame) : BView(frame, "game", B_FOLLOW_ALL, B_WILL_DRAW), fBitmap(NULL), fWidth(0), fHeight(0) { SetViewColor(B_TRANSPARENT_COLOR); }
	~GameView() { delete fBitmap; }
	void AttachedToWindow() { MakeFocus(true); }
	void Draw(BRect)
	{
		if (fBitmap) DrawBitmap(fBitmap, fBitmap->Bounds(), Bounds());
		else { SetHighColor(0, 0, 0); FillRect(Bounds()); }
	}
	void Refresh()
	{
		static std::vector<uint32_t> pixels(640 * 480);
		int w = 0, h = 0;
		if (!vga_compose(pixels.data(), w, h)) return;
		if (!fBitmap || w != fWidth || h != fHeight) { delete fBitmap; fBitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGB32); fWidth = w; fHeight = h; }
		for (int y = 0; y < h; ++y) memcpy(static_cast<uint8_t*>(fBitmap->Bits()) + y * fBitmap->BytesPerRow(), pixels.data() + y * w, w * 4);
		Invalidate();
	}
	void KeyDown(const char*, int32) { Key(true); }
	void KeyUp(const char*, int32) { Key(false); }
	void MessageReceived(BMessage* m)
	{
		if (m->what == B_MODIFIERS_CHANGED) Modifiers(m);
		else BView::MessageReceived(m);
	}
	// Shift, Control and Alt never arrive as key events on Haiku; turn
	// modifier transitions into the corresponding raw key presses.
	void Modifiers(BMessage* m)
	{
		int32 now = 0, old = 0;
		if (m->FindInt32("modifiers", &now) != B_OK) return;
		m->FindInt32("be:old_modifiers", &old);
		static const struct { uint32 mask; uint32 key; } kMap[] = {
			{ B_LEFT_SHIFT_KEY, 0x4b }, { B_RIGHT_SHIFT_KEY, 0x56 },
			{ B_LEFT_CONTROL_KEY, 0x5c }, { B_RIGHT_CONTROL_KEY, 0x60 },
			{ B_LEFT_COMMAND_KEY, 0x5d }, { B_RIGHT_COMMAND_KEY, 0x5f },
		};
		for (size_t i = 0; i < sizeof kMap / sizeof kMap[0]; ++i) {
			bool was = (old & kMap[i].mask) != 0, is = (now & kMap[i].mask) != 0;
			if (was != is) SendKey(kMap[i].key, is);
		}
	}
	void ReleaseAll()
	{
		for (uint32 k = 0; k < 0x80; ++k) if (fHeld[k]) SendKey(k, false);
	}
private:
	void Key(bool down)
	{
		BMessage* m = Window()->CurrentMessage(); int32 key = 0;
		if (m && m->FindInt32("key", &key) == B_OK) SendKey(uint32(key), down);
	}
	void SendKey(uint32 key, bool down)
	{
		if (key >= 0x80) return;
		if (!down && !fHeld[key]) return;
		fHeld[key] = down;
		input_key(key, down);
	}
	bool fHeld[0x80] = {};
	BBitmap* fBitmap; int fWidth, fHeight;
};

class GameWindow : public BWindow {
public:
	GameWindow() : BWindow(BRect(60, 60, 60 + 639, 60 + 399), "Prehistorik", B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE | B_ASYNCHRONOUS_CONTROLS)
	{
		// PREH_WINDOW="x,y" places the window; used by the remote test script.
		if (const char* pos = getenv("PREH_WINDOW")) { int x = 60, y = 60; if (sscanf(pos, "%d,%d", &x, &y) == 2) MoveTo(float(x), float(y)); }
		SetSizeLimits(320, 4096, 200, 4096);
		SetZoomLimits(1280, 800);
		fView = new GameView(Bounds()); AddChild(fView);
	}
	bool QuitRequested() { g.quitting = true; be_app->PostMessage(B_QUIT_REQUESTED); return true; }
	void WindowActivated(bool active) { if (!active) fView->ReleaseAll(); }
	void MessageReceived(BMessage* m)
	{
		if (m->what == 'tick') fView->Refresh();
		else if (m->what == B_MODIFIERS_CHANGED) fView->Modifiers(m);
		else BWindow::MessageReceived(m);
	}
	GameView* fView;
};

int32 gameThreadEntry(void*)
{
	gameMain();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return 0;
}

void joinThread(thread_id t)
{
	if (t < 0) return;
	status_t result;
	if (wait_for_thread_etc(t, B_RELATIVE_TIMEOUT, 3000000, &result) == B_TIMED_OUT)
		kill_thread(t);
}

// Every thread that touches the machine, the window or the sound player has
// to be gone before main() returns: the process exit destroys the globals
// they use, and a sound player that is still running crashes the media kit's
// control thread (BTimeSource::GetTime on a freed time source).
void shutdownMachine()
{
	g.quitting = true;
	joinThread(gameThread);
	joinThread(pit_thread());
	joinThread(displayThread);
	sound_shutdown();
}

int32 displayLoop(void* w)
{
	BMessenger target(static_cast<BWindow*>(w));
	while (!g.quitting) { snooze(1000000 / 60); target.SendMessage('tick'); }
	return 0;
}

// Test hook: PREH_KEYS="5e:3000,63:5000:2000" presses Haiku key codes (hex) at
// millisecond offsets, optionally held for the third value in ms.
int32 autoKeys(void*)
{
	const char* spec = getenv("PREH_KEYS"); if (!spec) return 0;
	bigtime_t start = system_time();
	std::string list(spec); size_t p = 0;
	while (p < list.size()) {
		size_t q = list.find(',', p); if (q == std::string::npos) q = list.size();
		std::string item = list.substr(p, q - p); p = q + 1;
		size_t colon = item.find(':'); if (colon == std::string::npos) continue;
		unsigned ms = unsigned(strtoul(item.c_str() + colon + 1, NULL, 10)), hold = 120;
		size_t colon2 = item.find(':', colon + 1);
		if (colon2 != std::string::npos) hold = unsigned(strtoul(item.c_str() + colon2 + 1, NULL, 10));
		std::vector<unsigned> keys; std::string ks = item.substr(0, colon);
		for (size_t k = 0; k < ks.size();) { size_t plus = ks.find('+', k); if (plus == std::string::npos) plus = ks.size(); keys.push_back(unsigned(strtoul(ks.substr(k, plus - k).c_str(), NULL, 16))); k = plus + 1; }
		while (system_time() - start < bigtime_t(ms) * 1000 && !g.quitting) snooze(20000);
		if (g.quitting) break;
		for (size_t k = 0; k < keys.size(); ++k) { input_key(keys[k], true); snooze(20000); }
		bigtime_t until = system_time() + bigtime_t(hold) * 1000;
		while (system_time() < until && !g.quitting) snooze(20000);
		for (size_t k = keys.size(); k-- > 0;) { input_key(keys[k], false); snooze(20000); }
	}
	return 0;
}

class App : public BApplication {
public:
	App(const std::string& exe, bool registerOnly) : BApplication("application/x-vnd.rainygirl-prehistorik"), fExe(exe), fWindow(NULL), fRegisterOnly(registerOnly) {}
	void ReadyToRun()
	{
		// --register only lets the registrar pick up the signature and icon,
		// which is what Tracker draws for the application and for links to it.
		if (fRegisterOnly) { PostMessage(B_QUIT_REQUESTED); return; }
		std::string error;
		if (!Prepare(error)) {
			// Asynchronous: a blocking Go() would stop the application from
			// answering B_QUIT_REQUESTED while the alert is on screen.
			BAlert* alert = new BAlert("Prehistorik", error.c_str(), "Quit");
			alert->Go(new BInvoker(new BMessage(B_QUIT_REQUESTED), be_app));
			return;
		}
		fWindow = new GameWindow(); fWindow->Show(); fWindow->Activate(true);
		displayThread = spawn_thread(displayLoop, "display", B_DISPLAY_PRIORITY, fWindow);
		resume_thread(displayThread);
		// Slightly above normal: the game must keep up with the music timer
		// (about 1.3 kHz) even while other applications are busy.
		gameThread = spawn_thread(gameThreadEntry, "prehistorik", B_DISPLAY_PRIORITY, NULL);
		resume_thread(gameThread);
		resume_thread(spawn_thread(autoKeys, "autokeys", B_NORMAL_PRIORITY, NULL));
	}
	bool Prepare(std::string& error)
	{
		if (fExe.empty()) {
			app_info info; GetAppInfo(&info); BPath p(&info.ref); p.GetParent(&p);
			const char* candidates[] = { "historik.exe", "original/historik.exe", "data/historik.exe", "HISTORIK.EXE" };
			for (size_t i = 0; i < 4; ++i) { BPath c(p.Path(), candidates[i]); struct stat st; if (stat(c.Path(), &st) == 0) { fExe = c.Path(); break; } }
			if (fExe.empty()) { error = "historik.exe not found next to the application (or in original/). Pass its path as the first argument."; return false; }
		}
		g.exePath = fExe;
		size_t s = fExe.find_last_of('/'); g.gameDir = s == std::string::npos ? "." : fExe.substr(0, s);
		if (getenv("PREH_TRACE")) { g.trace = true; const char* f = getenv("PREH_TRACE_FILE"); if (f) { g.traceFile = fopen(f, "w"); if (g.traceFile) setvbuf(g.traceFile, NULL, _IOLBF, 0); } }
		return machineInit(error);
	}
	// Sound must stop before the application tears down, or the sound player's
	// control thread keeps calling into code that is going away.
	bool QuitRequested()
	{
		g.quitting = true;
		sound_shutdown();
		return BApplication::QuitRequested();
	}
	std::string fExe; GameWindow* fWindow; bool fRegisterOnly;
};
}

namespace {

int32 dumpGameThread(void*)
{
	gameMain();
	return 0;
}

// --dump-intro: runs the game without a window or audio until the thought
// bubble screen of the intro is shown, and writes that 320x200 frame as a
// PPM. The build turns the ham in the bubble into the application icon, so
// no artwork from the game has to be stored in the source tree.
int dumpIntro(const std::string& out, const std::string& exe, const std::string& config)
{
	if (exe.empty()) { fprintf(stderr, "--dump-intro needs the path to historik.exe\n"); return 2; }
	g.exePath = exe;
	size_t s = exe.find_last_of('/');
	g.gameDir = s == std::string::npos ? "." : exe.substr(0, s);
	g.noAudio = true;
	g.configPath = config;
	std::string error;
	if (!machineInit(error)) { fprintf(stderr, "prehistorik: %s\n", error.c_str()); return 1; }
	gameThread = spawn_thread(dumpGameThread, "prehistorik", B_NORMAL_PRIORITY, NULL);
	resume_thread(gameThread);

	std::vector<uint32_t> frame(640 * 480), previous;
	bigtime_t deadline = system_time() + 120000000;
	int result = 1, w = 0, h = 0;
	while (system_time() < deadline && !g.quitting) {
		snooze(250000);
		// vga_compose() leaves the buffer and size alone when nothing changed,
		// so the last composed frame stays valid between polls.
		int nw = 0, nh = 0;
		if (vga_compose(frame.data(), nw, nh)) { w = nw; h = nh; }
		if (w != 320 || h != 200) { previous.clear(); continue; }
		// The bubble screen is the first 320x200 picture with a large
		// light-grey area in its upper part; wait until it is stable so a
		// palette fade is not captured half way.
		int light = 0;
		for (int y = 0; y < 70; ++y)
			for (int x = 80; x < 320; ++x)
				if ((frame[y * 320 + x] & 0xffffff) == 0xe2e2e2) ++light;
		std::vector<uint32_t> current(frame.begin(), frame.begin() + 320 * 200);
		if (light > 5000 && current == previous) {
			FILE* f = fopen(out.c_str(), "wb");
			if (!f) { fprintf(stderr, "cannot write %s\n", out.c_str()); break; }
			fprintf(f, "P6\n320 200\n255\n");
			for (int i = 0; i < 320 * 200; ++i) {
				uint8_t rgb[3] = { uint8_t(current[i] >> 16), uint8_t(current[i] >> 8), uint8_t(current[i]) };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
			fprintf(stderr, "prehistorik: wrote intro frame to %s\n", out.c_str());
			result = 0;
			break;
		}
		previous = light > 5000 ? current : std::vector<uint32_t>();
	}
	if (result != 0) fprintf(stderr, "prehistorik: the intro screen did not appear\n");
	g.quitting = true;
	joinThread(gameThread);
	joinThread(pit_thread());
	return result;
}

}

int main(int argc, char** argv)
{
	// Usage: Prehistorik [--setup] [--register] [path/to/historik.exe]
	//        Prehistorik --dump-intro <out.ppm> [--config <grawaga.cfg>] <path/to/historik.exe>
	// --setup passes "R" to the game, which clears the configuration and shows
	// the original setup screens (language, graphics, sound, controls).
	// --register starts and quits without a window, to register the signature.
	std::string exe, dumpPath, config;
	bool registerOnly = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--setup") == 0) g.commandTail = " R";
		else if (strcmp(argv[i], "--register") == 0) registerOnly = true;
		else if (strcmp(argv[i], "--dump-intro") == 0 && i + 1 < argc) dumpPath = argv[++i];
		else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config = argv[++i];
		else exe = argv[i];
	}
	if (!dumpPath.empty()) return dumpIntro(dumpPath, exe, config);
	App app(exe, registerOnly);
	app.Run();
	shutdownMachine();
	return g.exitCode;
}
