# Native Haiku port of Prehistorik (DOS). Build on Haiku x86 (32-bit) with:
#   setarch x86 make GAME=/path/to/your/prehistorik
#
# GAME is the directory holding your copy of the game (historik.exe,
# filesa.cur, filesa.vga, filesb.cur, filesb.vga). It is needed to generate
# the translated sources (make translate, on a machine with Python 3 and
# iced-x86) and to trace the application icon from the intro screen.
GAME ?= original
CC ?= gcc
CXX ?= g++
CFLAGS ?= -O1 -std=gnu11 -m32 -Wall -Wno-unused-label -Wno-parentheses -fno-strict-aliasing
CXXFLAGS ?= -O2 -std=c++14 -m32 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing
BUILD ?= build
GEN_SRC = $(wildcard src/gen/seg_*.c) src/gen/tables.c
GEN_OBJ = $(patsubst src/gen/%.c,$(BUILD)/gen_%.o,$(GEN_SRC))
RT_SRC = src/runtime.cpp src/vga.cpp src/sound.cpp src/app.cpp
RT_OBJ = $(patsubst src/%.cpp,$(BUILD)/%.o,$(RT_SRC))
YMFM_SRC = src/ymfm/ymfm_opl.cpp src/ymfm/ymfm_adpcm.cpp src/ymfm/ymfm_pcm.cpp
YMFM_OBJ = $(patsubst src/ymfm/%.cpp,$(BUILD)/ymfm_%.o,$(YMFM_SRC))
ifdef DEBUG
CFLAGS += -DTRACE
endif

.PHONY: all clean distclean translate icon install
all: $(BUILD)/Prehistorik icon

$(BUILD):
	mkdir -p $(BUILD)

# The translated game code is generated from your historik.exe and is not
# part of the repository.
src/gen/tables.c:
	@echo "src/gen/ is missing. Generate it from your copy of the game first:"
	@echo "    python3 tools/translate.py /path/to/historik.exe src/gen"
	@echo "This needs Python 3 with iced-x86 (see README.md)."
	@exit 1

$(BUILD)/gen_%.o: src/gen/%.c src/cpu.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/%.o: src/%.cpp src/machine.h src/cpu.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

$(BUILD)/ymfm_%.o: src/ymfm/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Wno-unused-parameter -c $< -o $@

$(BUILD)/Prehistorik: $(GEN_OBJ) $(RT_OBJ) $(YMFM_OBJ) $(BUILD)/Prehistorik.rsrc
	$(CXX) $(CXXFLAGS) $(GEN_OBJ) $(RT_OBJ) $(YMFM_OBJ) -o $@ -lbe -lmedia -lroot
	xres -o $@ $(BUILD)/Prehistorik.rsrc
	resattr -o $@ $(BUILD)/Prehistorik.rsrc
	mimeset -f $@
	addattr -t mime BEOS:TYPE application/x-vnd.be-elfexecutable $@

$(BUILD)/Prehistorik.rsrc: resources/Prehistorik.rdef | $(BUILD)
	rc -o $@ $<

# The icon is the ham from the intro's thought bubble. It is traced from the
# game at build time (the binary captures the intro frame without opening a
# window), then attached together with the other resources. Without the game
# files the application simply keeps the generic icon.
icon: $(BUILD)/Prehistorik
	@if [ ! -f $(BUILD)/icon.rdef ]; then \
		if [ ! -f "$(GAME)/historik.exe" ]; then \
			echo "note: $(GAME)/historik.exe not found, building without the icon (make GAME=/path/to/game)"; \
			exit 0; \
		fi; \
		echo "capturing the intro screen for the icon..."; \
		$(BUILD)/Prehistorik --dump-intro $(BUILD)/intro.ppm --config resources/grawaga.cfg "$(GAME)/historik.exe" && \
		python3 tools/make_icon.py $(BUILD)/intro.ppm $(BUILD)/icon.rdef || exit 1; \
	fi; \
	if [ ! -f $(BUILD)/icon.stamp ] || [ $(BUILD)/Prehistorik -nt $(BUILD)/icon.stamp ]; then \
		rc -o $(BUILD)/Prehistorik-icon.rsrc resources/Prehistorik.rdef $(BUILD)/icon.rdef && \
		xres -o $(BUILD)/Prehistorik $(BUILD)/Prehistorik-icon.rsrc && \
		resattr -o $(BUILD)/Prehistorik $(BUILD)/Prehistorik-icon.rsrc && \
		mimeset -f $(BUILD)/Prehistorik && \
		addattr -t mime BEOS:TYPE application/x-vnd.be-elfexecutable $(BUILD)/Prehistorik && \
		touch $(BUILD)/icon.stamp; \
	fi

translate:
	python3 tools/translate.py "$(GAME)/historik.exe" src/gen

# Installs into ~/config/non-packaged/apps/Prehistorik with Deskbar and
# Desktop links and a `prehistorik` command in ~/config/non-packaged/bin.
# The game files are copied next to the application; an existing
# grawaga.cfg (your settings) is kept.
APPDIR ?= /boot/home/config/non-packaged/apps/Prehistorik
GAME_FILES = historik.exe filesa.cur filesa.vga filesb.cur filesb.vga
install: all
	@for f in $(GAME_FILES); do \
		if [ ! -f "$(GAME)/$$f" ]; then echo "$(GAME)/$$f not found (make install GAME=/path/to/game)"; exit 1; fi; \
	done
	@hey Prehistorik quit >/dev/null 2>&1 || true
	mkdir -p "$(APPDIR)"
	cp $(BUILD)/Prehistorik "$(APPDIR)/Prehistorik"
	for f in $(GAME_FILES); do cp "$(GAME)/$$f" "$(APPDIR)/$$f"; done
	[ -f "$(APPDIR)/grawaga.cfg" ] || cp resources/grawaga.cfg "$(APPDIR)/grawaga.cfg"
	chmod u+w "$(APPDIR)/grawaga.cfg"
	addattr -t mime BEOS:TYPE application/x-vnd.be-elfexecutable "$(APPDIR)/Prehistorik"
	mkdir -p /boot/home/config/settings/deskbar/menu/Applications
	@# Keep links that already point here: replacing one makes Tracker drop
	@# the Desktop icon until it restarts.
	mkdir -p /boot/home/config/non-packaged/bin
	@for l in /boot/home/config/settings/deskbar/menu/Applications/Prehistorik /boot/home/Desktop/Prehistorik /boot/home/config/non-packaged/bin/prehistorik; do \
		[ "$$(readlink "$$l")" = "$(APPDIR)/Prehistorik" ] || ln -sf "$(APPDIR)/Prehistorik" "$$l"; \
	done
	rm -f /boot/home/config/settings/mime_db/application/x-vnd.rainygirl-prehistorik
	"$(APPDIR)/Prehistorik" --register
	@echo "Installed to $(APPDIR). Start it from the Deskbar, the Desktop, or by typing: prehistorik"
	@echo "If the Desktop icon is missing or generic, restart Tracker: hey Tracker quit; /boot/system/Tracker &"

clean:
	rm -f $(BUILD)/*.o $(BUILD)/Prehistorik $(BUILD)/*.rsrc $(BUILD)/icon.rdef $(BUILD)/icon.stamp $(BUILD)/intro.ppm

# Also removes the sources generated from the game.
distclean: clean
	rm -rf src/gen
