# Sokol OS -- one C application, one window, one game.
#
#   make            build bin/sokol-os (needs a window system)
#   make run        build and launch it
#   make frames     render PNG screenshots headlessly into build/frames
#   make check      build the screenshots and verify they exist
#   make clean      drop build/ and bin/

CC ?= cc
CFLAGS ?= -O2
PYTHON ?= python3

WARNINGS := -Wall -Wextra -Wpedantic -Werror
# The sokol headers are written against every compiler in existence and do
# not survive -Wpedantic -Werror; everything we own is compiled strictly,
# the two files that embed third-party implementation are compiled relaxed.
STRICT := $(WARNINGS) -Wno-unused-parameter
RELAXED := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function

INCLUDES := -Isrc -Ithird_party/sokol -Ithird_party/stb -Ibuild
STD := -std=c11
MATH := -lm
# Let the compiler write header dependencies so editing a header (including
# the generated font table) rebuilds exactly what uses it.
DEPFLAGS := -MMD -MP

UNAME := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME),Darwin)
    PLATFORM_LIBS := -framework Cocoa -framework QuartzCore -framework OpenGL -framework IOKit
    APP_SUFFIX :=
    # sokol_app's macOS backend is Objective-C (NSWindow, NSApplication),
    # so the one file that includes its implementation must be compiled as
    # Objective-C. Everything else stays plain C11.
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
APP := $(BIN)/sokol-os$(APP_SUFFIX)
RENDER := $(BIN)/render-frames$(APP_SUFFIX)
FRAMES := $(BUILD)/frames
FONT_HEADER := $(BUILD)/font_data.h
FONT_SOURCE := fonts/font.ttf

CORE_SOURCES := src/app.c src/snake.c
CORE_OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SOURCES))
DRAW_OBJECT := $(BUILD)/draw.o
PLATFORM_OBJECT := $(BUILD)/platform_sokol.o
RENDER_OBJECT := $(BUILD)/render_frames.o

.PHONY: all app run render frames check clean font

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
	$(CC) $(STD) $(CFLAGS) $(STRICT) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/draw.o: src/draw.c src/draw.h src/mono_font.h | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(RELAXED) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/platform_sokol.o: src/platform_sokol.c $(FONT_HEADER) | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(RELAXED) $(DEPFLAGS) $(PLATFORM_LANG) $(INCLUDES) -c $< -o $@

$(RENDER_OBJECT): tools/render_frames.c | $(BUILD)
	$(CC) $(STD) $(CFLAGS) $(RELAXED) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(APP): $(CORE_OBJECTS) $(DRAW_OBJECT) $(PLATFORM_OBJECT) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(MATH) $(PLATFORM_LIBS)

$(RENDER): $(CORE_OBJECTS) $(DRAW_OBJECT) $(RENDER_OBJECT) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(MATH)

render: $(RENDER)

frames: $(RENDER)
	@mkdir -p $(FRAMES)
	$(RENDER) $(FONT_SOURCE) $(FRAMES)

check: frames
	@test -s $(FRAMES)/03-desktop.png
	@test -s $(FRAMES)/05-snake-play.png
	@echo "frames ok: $(FRAMES)"

run: $(APP)
	./$(APP)

font: | $(BUILD)
	$(PYTHON) tools/make_mono_font.py src/mono_font.h

clean:
	rm -rf $(BUILD) $(BIN)

-include $(wildcard $(BUILD)/*.d)
