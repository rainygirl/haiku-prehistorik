# Prehistorik for Haiku

[한국어](README.ko.md)

![Prehistorik running on Haiku](docs/screenshots/gameplay.png)

A native Haiku port of the DOS game *Prehistorik* (Titus, 1991). The game's
own code is translated to C and compiled into a Haiku application, so it
runs without a DOS emulator. It starts in VGA mode with AdLib music and
Sound Blaster effects played through the Haiku media kit, and it is played
with the keyboard.

**This repository contains none of the game's files or code.** You need your
own copy of the DOS release. The translated code, the icon and everything
else derived from the game are generated on your machine from your files.

## What you need

* **A Haiku PC**, 32-bit x86 (x86_gcc2 hybrid) with the x86 development
  tools (`setarch x86`). Tested on Haiku R1/beta6 (development build
  hrev99002).
* **Your copy of Prehistorik** for DOS, with these five files:
  `historik.exe`, `filesa.cur`, `filesa.vga`, `filesb.cur`, `filesb.vga`.
  The port was made for the release whose `historik.exe` is 164,166 bytes.
* **A Linux, macOS or Windows computer with Python 3**, used once to
  translate `historik.exe` into C. The translator needs the `iced-x86`
  package, which is not available on Haiku.

## 1. Get the source

```sh
git clone https://github.com/rainygirl/haiku-prehistorik.git
cd haiku-prehistorik
```

## 2. Translate the game code (Linux, macOS or Windows)

```sh
python3 -m pip install iced-x86
python3 tools/translate.py /path/to/prehistorik/historik.exe src/gen
```

This writes about 70 C files to `src/gen/`. They are made from your copy of
the game, so do not publish them. `.gitignore` already excludes them.

## 3. Build on Haiku

Copy the whole `haiku-prehistorik` folder, including `src/gen/`, to the
Haiku machine. Then, in Terminal:

```sh
cd haiku-prehistorik
setarch x86 make GAME=/path/to/prehistorik
```

`GAME` is the folder with your five game files. The build compiles the
translated code, then starts the game once without a window to capture the
intro screen and turn the ham in the caveman's thought bubble into the
application icon. A full build takes about 10 minutes on a slow machine.

## 4. Install

```sh
setarch x86 make install GAME=/path/to/prehistorik
```

This copies the application and the game files to
`/boot/home/config/non-packaged/apps/Prehistorik`, and adds Prehistorik to
the Deskbar's Applications menu and to the Desktop.

If the Prehistorik icon is missing from the Desktop or looks generic,
Tracker is showing a cached view. Restart Tracker, which only closes Tracker's own windows:

```sh
hey Tracker quit; /boot/system/Tracker &
```

## Running

Start Prehistorik from the Deskbar or the Desktop. You can also run the
build directly:

```sh
build/Prehistorik /path/to/prehistorik/historik.exe
```

Without an argument the application looks for `historik.exe` in its own
folder.

| Key | Action |
| --- | --- |
| Left / Right | Walk |
| Up | Jump |
| Down | Crouch, enter caves |
| Space | Swing the club, start the game |
| Esc | Quit the game |

The picture scales with the window. The installed `grawaga.cfg` selects
English, VGA, Sound Blaster and the keyboard, so the game goes straight to
the intro. To choose another language, graphics mode, sound device or
controller, run the game from Terminal with the original setup screens:

```sh
/boot/home/config/non-packaged/apps/Prehistorik/Prehistorik --setup
```

## License

The port's own code is released under the MIT License (see `LICENSE`).
Third-party components keep their own licenses: `src/ymfm` is Aaron Giles'
ymfm (BSD 3-Clause), and the 8x8 font comes from Daniel Hepper's
public-domain `font8x8`. See `THIRD_PARTY_NOTICES.md`.

*Prehistorik* is © 1991 Titus Software. None of its files or code is
included here, and the MIT License does not cover the game or anything
generated from it. The two images in `docs/screenshots/`, a gameplay
screenshot and a preview of the icon, show the game's artwork only to
illustrate this port.

---

This port was developed with AI assistance (Claude by Anthropic).
