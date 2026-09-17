# Development notes

## Architecture

`historik.exe` is a 16-bit real-mode Turbo C/Pascal-style MZ program
(155 KB image, 1917 relocations, 69 code segments). It is not emulated
at run time. It is statically recompiled:

1. `tools/mz.py` loads the image at segment 0100h and applies relocations.
2. `tools/listing.py` finds code by recursive descent from the entry point.
   It follows `jmp [cs:bx+table]` switch tables, far pointers built as
   `mov ax,off / mov dx,seg / push dx / push ax` (interrupt handlers), and
   seeds from `tools/seeds.txt` (procedure pointers stored in data, found
   at run time). It writes `analysis/listing.asm`.
3. `tools/translate.py` turns every instruction into C (`src/gen/seg_XXXX.c`,
   one function per CS value). Real 8086 flags are computed by the helpers
   in `src/cpu.h`. Near jumps inside a segment are `goto`. Near indirect
   jumps and returns use a computed-goto table (`NEAR_DISPATCH`). Far
   transfers return the next linear address to the dispatcher in
   `src/runtime.cpp`. Bytes discovery never reached are linearly swept and
   translated too, so rare paths do not abort. Undecodable junk becomes a
   `fatal()` stub.
4. `irq_check()` runs at every backward branch, call, `sti` and `popf`.
   Hardware interrupts (PIT IRQ0, keyboard IRQ1) are delivered there by
   running the game's own handler through the IVT.

Machine memory is a flat 1 MB array. `A0000-BFFFF` goes through the VGA
emulation. The IVT points at `F000:n` for native BIOS/DOS services.

## Native services (`src/runtime.cpp`)

* DOS `int 21h`: files map case-insensitively onto the game directory.
  Memory calls and the PSP/environment are enough for the Turbo runtime.
* PIT: a thread raises IRQ0 at the programmed channel-0 rate (the music
  driver sets about 1 kHz). Port 40h-43h reads are computed from wall time.
* Keyboard: Haiku key codes become set-1 scancodes on port 60h plus a BIOS
  `int 16h` buffer.
* Turbo Pascal `Intr()` builds `push bp; int n; pop bp; retf` on the stack.
  The dispatcher recognises that pattern instead of translating stack bytes.
* CT-VOICE.DRV is not shipped with the game. The far call into the loaded
  driver (`117D:0000`) is replaced by `ctvoice_call()` during translation,
  and opening `ct-voice.drv` serves a small file with the `CT-VOICE`
  signature the game checks. That file, like the scratch `grawaga.cfg` of
  `--dump-intro`, is created in `/tmp` and deleted right after it is opened,
  so no temporary files are left behind (earlier builds left one per launch).

## Video (`src/vga.cpp`)

The VGA path uses EGA mode 0Dh (320x200, 16 colours) with page flipping via
`int 10h AH=05h`, which moves the CRTC start address. On a VGA, 200-line
modes map attribute colours 8-15 to DAC 10h-17h. The game writes its
palette there through ports 3C8h/3C9h. Planar write modes 0-3, read modes,
latches, map mask, set/reset and bit mask are implemented. Text mode uses a
CP437 8x8 font for the setup menu.

## Sound (`src/sound.cpp`)

* OPL2 at 388h through ymfm (`ym3812`). The AdLib detection does 200 status
  reads and expects the 80 us timer to have expired. Natively these reads
  finish in microseconds, so every status read advances the chip clock by
  4 ticks.
* VOC effects (`BONUS.VOC`, `MASSUE2.VOC`, `RESSORT.VOC`) are decoded from
  game memory when CT-VOICE function 6 is called. The status word is
  cleared when playback ends.
* PC speaker (PIT channel 2 plus port 61h) for the "IBM" sound option.
* One `BSoundPlayer` mixes everything. `PREH_PCM=file` dumps the mix
  (16-bit stereo, 49716 Hz). `PREH_NOSOUND=1` keeps the mix but sends
  silence to the device. Use it for test runs on machines next to people.

## Configuration

`grawaga.cfg` is 7 bytes: language, graphics (`C E V H T`), sound
(`A I S R N`), controls (`C J M`), SB port (word) and SB IRQ. Each setup
screen is skipped when its value is valid. Sound `A` needs AdLib detection,
`S` needs the CT-VOICE signature, and `R` needs an MPU-401. The game probes
video cards by setting modes 12h, 13h, 10h and 04h and reading the mode back
(`0DFA:022B`). A command-line argument `R` clears the loaded configuration.
The app's `--setup` flag puts ` R` in the PSP command tail (`PREH_TAIL` in
the host build).

## Testing without Haiku

`tools/host/` builds the same runtime headless on macOS/Linux.
`tools/host/include/OS.h` is a shim for the kernel kit.

```sh
tools/rebuild-host.sh
PREH_NOSOUND=1 build/host/preh-host build/host/game/historik.exe 40 \
    6000,24000 5e:20000,63:22000:3000
```

The arguments are the game exe, the seconds to run, the screenshot times in
ms (PNG files in `build/host/`), and key events as Haiku key code in hex,
`:ms` and an optional `:hold_ms`. Useful codes: Space 5e, Enter 47, Esc 01,
Left 61, Right 63, Up 57, Down 62. `PREH_TRACE=1 PREH_TRACE_FILE=...` logs
DOS, BIOS, port and mode events. The macOS 27 SDK tbd files break
linking, so the host Makefile links against the 26.5 SDK.

Building on the Haiku box (Atom Z520) takes about 10 minutes from scratch,
mostly the generated C.
When a new "no code at XXXX:YYYY" appears, add it to `tools/seeds.txt`
and retranslate.

## Icon

The icon is the roast ham from the thought bubble on the fourth intro
screen. That artwork belongs to Titus, so the repository does not contain
it; the build captures it from the user's game files:

1. `Prehistorik --dump-intro build/intro.ppm --config resources/grawaga.cfg
   GAME/historik.exe` runs the translated game without a window or audio
   device. It polls the composed frame and writes the first stable 320x200
   frame with a large light-grey area in the upper part (the bubble). With
   `--config`, the game reads its configuration from the repository and its
   rewrite of `grawaga.cfg` goes to a scratch file, so the game directory is
   not modified. `BULLE.PC1` itself is compressed in a Titus format that
   was not reverse engineered; letting the game decode it was simpler.
2. `tools/make_icon.py build/intro.ppm build/icon.rdef` crops the ham at
   (174,14)-(226,55), removes the bubble by flood filling from the crop's
   edges, turns grey pixels touching the bubble into a translucent shadow,
   and traces every same-coloured 4-connected region into one orthogonal
   polygon. Regions are painted largest first, so anything inside another
   region's hole is drawn over it. Result: 222 paths, 6,982 bytes, under
   HVIF's limit of 255 paths.
3. The `icon` target compiles `resources/Prehistorik.rdef` together with
   `build/icon.rdef` and attaches both with `xres`/`resattr`.

The output was checked byte for byte against the icon made from the
embedded pixel map this replaced. Without game files the build keeps the
generic icon and prints a note.

## Publishing

`.gitignore` keeps out everything that is the game or derived from it:
`original/` and the game file names, `src/gen/` (C translated from
`historik.exe`), `analysis/` (disassembly), `build/` (translated objects,
intro captures, the traced icon), PPM and PCM dumps and crash reports.
`docs/screenshots/` is committed on purpose: one gameplay screenshot and a
preview of the icon, used by the README to show the port. Before a release, check with a scratch clone that
`git ls-files` shows only the port, and that no tracked file contains
strings or 32-byte runs of the game data. `tools/translate.py` warns when
`historik.exe` is not the release this port was made for (sha256 in
`KNOWN_SHA256`). The remote scripts (`deploy.sh`, `install.sh`,
`run-remote.sh`, `quit-stress.sh`) take the Haiku machine from
`HAIKU_HOST`.

## Shutdown

Closing the window or `hey Prehistorik quit` used to crash on renku: `main()`
returned while the game thread, the timer thread and the `BSoundPlayer` were
still running, and the media kit's control thread faulted in
`BTimeSource::GetTime` while the process was exiting. Now `irq_deliver()` and
`idle_spin()` leave the program with `longjmp` as soon as `g.quitting` is
set, and `main()` joins the game, timer and display threads and stops the
sound player (`sound_shutdown()` runs once, whichever thread gets there
first) before returning. `tools/quit-stress.sh` launches the installed app
eight times, quits it through the application and through the window at
different moments, and counts new crash reports.

## Environment variables

| Variable | Effect |
| --- | --- |
| `PREH_NOSOUND=1` | Mix as usual but send silence to the device |
| `PREH_PCM=file` | Dump the mix (16-bit stereo, 49715 Hz) |
| `PREH_STATS=1` | Log IRQ0 raised/delivered, IRQ1 and frames every 5 s |
| `PREH_TRACE=1`, `PREH_TRACE_FILE=path` | Log DOS/BIOS/port/mode events |
| `PREH_KEYS=5e:20000,63:32000:8000` | Press Haiku key codes at ms offsets, optional hold |
| `PREH_WINDOW=x,y` | Place the window (remote screenshots) |
| `PREH_TAIL=" R"` | DOS command tail in the host build |

`HAIKU_HOST=user@host tools/run-remote.sh SECONDS 'ENV=...'` builds nothing,
runs the binary on the Haiku box and fetches a screenshot.
`tools/deploy.sh [sync-data]` builds there, `tools/install.sh` installs into
`~/config/non-packaged/apps/Prehistorik` with Deskbar and Desktop links and
restarts Tracker. On the Haiku machine itself, `make install GAME=...` does
the same without restarting Tracker.

## Haiku notes

* `int32` is `long` on x86 Haiku. The `atomic_*` calls need `int32*`, so
  `irq_pending` (declared `int32_t` for C) is cast (`IRQP`).
* Modifier keys never produce `KeyDown`. They come in
  `B_MODIFIERS_CHANGED` and are converted to Shift/Ctrl/Alt scancodes.
  Held keys are released when the window deactivates.
* A blocking `BAlert::Go()` in `ReadyToRun()` makes the application
  unquittable (`hey Prehistorik quit` hangs), so the startup error alert uses
  the asynchronous form.
* `tar` sets the extracted mtimes, so `resources/*.rdef` is touched before
  `make` on the host; otherwise the resources are not rebuilt.
* Three separate things make Tracker show a generic icon for the application:
  `mimeset` types the binary `application/octet-stream` (fix it with
  `addattr -t mime BEOS:TYPE application/x-vnd.be-elfexecutable`), and the
  icon Tracker uses for the app and for symlinks to it comes from the MIME
  database entry `~/config/settings/mime_db/application/<signature>`, which
  the registrar writes on the app's first run and never refreshes. Deleting
  that file and launching the app again registers the current icon; the
  install script does this with `Prehistorik --register`, which starts and
  quits without opening a window. Links themselves need no attributes.
  Finally, Tracker caches the icon it resolved for an application signature
  in memory. A folder window draws the binary's own icon correctly, but every
  freshly created link to it keeps drawing the cached generic icon, and
  neither re-registering nor running the app clears it. Only restarting
  Tracker does (`hey Tracker quit` then `/boot/system/Tracker &`), which the
  install script now does at the end; it closes Tracker's own windows only.
  `tools/trackericon.cpp` prints the icon Tracker resolves for a path, with
  a fingerprint, which is easier than reading screenshots.
* The game thread runs at `B_DISPLAY_PRIORITY`. At normal priority the
  1.3 kHz music timer lost about 8 % of its ticks on the Atom box while a
  browser was running; at display priority the loss is about 1.5 %.
* Measured on renku (Atom Z520, 1.33 GHz): 57 frames/s composed, timer
  delivery 1103 of 1120 per second, full build about 10 minutes.
