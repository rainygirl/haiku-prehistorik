# Headless macOS/Linux build for testing: make -f tools/host/Makefile
HB = build/host
CC = cc
CXX = c++
CFLAGS = -O1 -std=gnu11 -w -Isrc
CXXFLAGS = -O1 -g -std=c++14 -Wall -Wno-unused-parameter -Wno-misleading-indentation -Isrc -Itools/host/include
GEN_SRC = $(wildcard src/gen/seg_*.c) src/gen/tables.c
GEN_OBJ = $(patsubst src/gen/%.c,$(HB)/gen_%.o,$(GEN_SRC))
RT_OBJ = $(HB)/runtime.o $(HB)/vga.o $(HB)/sound.o $(HB)/main.o
YMFM_OBJ = $(HB)/ymfm_opl.o $(HB)/ymfm_adpcm.o $(HB)/ymfm_pcm.o
all: $(HB)/preh-host
$(HB):
	mkdir -p $(HB)
$(HB)/gen_%.o: src/gen/%.c src/cpu.h | $(HB)
	$(CC) $(CFLAGS) $(EXTRA) -c $< -o $@
$(HB)/%.o: src/%.cpp src/machine.h src/cpu.h | $(HB)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(HB)/main.o: tools/host/main.cpp src/machine.h | $(HB)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(HB)/ymfm_%.o: src/ymfm/ymfm_%.cpp | $(HB)
	$(CXX) -O2 -std=c++14 -w -c $< -o $@
$(HB)/preh-host: $(GEN_OBJ) $(RT_OBJ) $(YMFM_OBJ)
	$(CXX) -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk $^ -o $@ -lz -lpthread
