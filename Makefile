CC ?= gcc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror
IMAGE_CHECKER := bin/dimos-image-check
FONT_PREVIEW := bin/dimos-font-preview

.PHONY: all iso tools verify font clean

all: iso

iso: tools
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

clean:
	rm -rf bin disk_img
