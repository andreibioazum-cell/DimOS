# DimOS -- one C program, one window, one game, rendered with sokol.
#
#   make            build bin/dimos (needs a window system while running)
#   make run        build and launch it
#   make iso        build dist/dimos.iso and dist/dimos.img (Arch or Docker)
#   make clean      drop build/, bin/ and dist/
#
# The ISO is an Arch Linux live image that boots straight into the DimOS
# desktop; see iso/README.md. `make iso` needs a Linux with archiso/Docker,
# which is what CI uses.

CC ?= cc
CFLAGS ?= -O2
PYTHON ?= python3

WARNINGS := -Wall -Wextra -Wpedantic -Werror
# The sokol headers are written for every compiler in existence and do not
# survive -Wpedantic -Werror; everything we own is compiled strictly, the two
# files that embed third-party implementation are compiled relaxed.
STRICT := $(WARNINGS) -Wno-unused-parameter
RELAXED := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function

INCLUDES := -Isrc -Ithird_party/sokol -Ithird_party/stb -Ibuild
# gnu11 rather than c11: sokol_app needs POSIX (clock_gettime, dlopen) and
# a strict -std=c11 would hide those declarations on glibc.
STD := -std=gnu11
MATH := -lm

UNAME := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME),Darwin)
    PLATFORM_LIBS := -framework Cocoa -framework QuartzCore -framework OpenGL -framework IOKit
    APP_SUFFIX :=
    # sokol_app's macOS backend is Objective-C (NSWindow, NSApplication), so
    # the one file that includes its implementation is compiled as ObjC.
    PLATFORM_LANG := -x objective-c
else ifeq ($(OS),Windows_NT)
    PLATFORM_LIBS := -lgl -lgdi32 -luser32 -lshell32 -lole32
    APP_SUFFIX := .exe
else
    PLATFORM_LIBS := -lX11 -lXi -lXcursor -lGL -ldl -lpthread
    APP_SUFFIX :=
endif

BUILD := build
BIN := bin
DIST := dist
APP := $(BIN)/dimos$(APP_SUFFIX)
FONT_HEADER := $(BUILD)/font_data.h
FONT_SOURCE := fonts/font.ttf

CORE_SOURCES := src/app.c src/snake.c
CORE_OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SOURCES))
DRAW_OBJECT := $(BUILD)/draw.o
PLATFORM_OBJECT := $(BUILD)/platform_sokol.o

.PHONY: all app run iso clean font

all: app

app: $(APP)

$(BUILD):
	@mkdir -p $(BUILD)

$(BIN):
	@mkdir -p $(BIN)

$(FONT_HEADER): $(FONT_SOURCE) tools/embed_font.py
	@mkdir -p $(BUILD)
	$(PYTHON) tools/embed_font.py $(FONT_SOURCE) $(FONT_HEADER)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(STRICT) -MMD -MP $(INCLUDES) -c $< -o $@

$(BUILD)/draw.o: src/draw.c src/draw.h src/mono_font.h | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(RELAXED) -MMD -MP $(INCLUDES) -c $< -o $@

$(BUILD)/platform_sokol.o: src/platform_sokol.c $(FONT_HEADER) | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(RELAXED) -MMD -MP $(PLATFORM_LANG) $(INCLUDES) -c $< -o $@

$(APP): $(CORE_OBJECTS) $(DRAW_OBJECT) $(PLATFORM_OBJECT) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(MATH) $(PLATFORM_LIBS)

run: $(APP)
	./$(APP)

# Builds the live ISO and the dd-able image. Runs on Arch Linux directly or
# through Docker on any other Linux; CI does the same in an archlinux
# container.
iso:
	./iso/build-iso.sh

font: | $(BUILD)
	$(PYTHON) tools/make_mono_font.py src/mono_font.h

clean:
	rm -rf $(BUILD) $(BIN) $(DIST)

-include $(wildcard $(BUILD)/*.d)
