CC ?= gcc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror
IMAGE_CHECKER := bin/dimos-image-check
FONT_PREVIEW := bin/dimos-font-preview

.PHONY: all iso emulator tools verify font clean anywhere anywhere-test lua-demo phone release

all: iso

iso: tools
	./build-linux.sh

# The economy profile needs both halves of the kernel to agree on it:
# C selects the full output area and the real-mode VBE code selects the
# smaller physical 640x480 mode. Passing the define only to C leaves a tiny
# viewport inside a 1920x1080 VBE screen.
emulator: tools
	KERNEL_CFLAGS="$(strip $(KERNEL_CFLAGS) -DDIMOS_EMULATOR)" \
	KERNEL_ASMFLAGS="$(strip $(KERNEL_ASMFLAGS) -DDIMOS_EMULATOR)" \
		./build-linux.sh

tools: $(IMAGE_CHECKER) $(FONT_PREVIEW)

# The artifact checker is plain C, like the kernel: no C++ anywhere.
$(IMAGE_CHECKER): tools/image_check.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 $(CFLAGS) $(WARNINGS) $< -o $@

# The font previewer compiles the kernel's own TrueType reader as a host
# program, so `make font` shows exactly what the kernel will draw.
$(FONT_PREVIEW): tools/font_preview.c src/kernel/font_ttf.c src/kernel/dimos.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 $(CFLAGS) $(WARNINGS) -DDIMOS_HOST_TEST \
		-I src/kernel tools/font_preview.c src/kernel/font_ttf.c -o $@

font: $(FONT_PREVIEW)
	$(FONT_PREVIEW) fonts/font.ttf

verify: tools
	$(IMAGE_CHECKER) bin/BOOT.BIN bin/KERNEL.BIN disk_img/dimos.img disk_img/dimos.hdd disk_img/dimos.iso

# The hosted runtime: the same kernel as an ordinary program for Linux,
# Windows and Android, with a browser front end and headless Lua.
anywhere:
	./build-anywhere.sh

anywhere-test: anywhere
	bin/dimos-anywhere --disk bin/anywhere.hdd --shots bin/anywhere-shots self-test

lua-demo: anywhere
	bin/dimos-anywhere --disk bin/anywhere.hdd run HELLO.LUA

# The phone runtime: the same sources, cross compiled for ARM64 and linked
# statically, so an Android phone starts it without a matching C library.
# Needs zig on PATH (pip install ziglang puts it in ~/.local/bin).
phone:
	./build-anywhere.sh --target aarch64-linux-musl

# Collect what has been built into release/PC, release/Emulator and
# release/Phone -- the three folders that are handed to the user.
release: anywhere phone
	tools/release.sh all

clean:
	rm -rf bin disk_img
