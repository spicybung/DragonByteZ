CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -Iinclude -Ithird_party/viogsf
LDLIBS ?=

VIOGSF = third_party/viogsf/vbam/apu/Blip_Buffer.cpp \
third_party/viogsf/vbam/apu/Effects_Buffer.cpp \
third_party/viogsf/vbam/apu/Gb_Apu.cpp \
third_party/viogsf/vbam/apu/Gb_Oscs.cpp \
third_party/viogsf/vbam/apu/Multi_Buffer.cpp \
third_party/viogsf/vbam/gba/bios.cpp \
third_party/viogsf/vbam/gba/GBA-arm.cpp \
third_party/viogsf/vbam/gba/GBA-thumb.cpp \
third_party/viogsf/vbam/gba/GBA.cpp \
third_party/viogsf/vbam/gba/Sound.cpp

CORE = src/analysis.cpp src/compression.cpp src/gba_bios.cpp src/png.cpp src/rom.cpp src/gsf_player.cpp src/log1_runtime.cpp src/sprite_analysis.cpp $(VIOGSF)

.PHONY: all clean test

all: dragonbytez-cli

dragonbytez-cli: src/main.cpp $(CORE)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-error $^ $(LDLIBS) -o $@

dragonbytez_tests: tests/tests.cpp $(CORE)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-error $^ $(LDLIBS) -o $@

test: dragonbytez_tests
	./dragonbytez_tests

clean:
	rm -f dragonbytez dragonbytez-cli dragonbytez_tests
