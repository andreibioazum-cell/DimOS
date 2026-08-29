CC ?= gcc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror
IMAGE_CHECKER := bin/dimos-image-check

.PHONY: all iso tools verify clean

all: iso

iso: tools
	./build-linux.sh

tools: $(IMAGE_CHECKER)

# The artifact checker is plain C, like the kernel: no C++ anywhere.
$(IMAGE_CHECKER): tools/image_check.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 $(CFLAGS) $(WARNINGS) $< -o $@

verify: tools
	$(IMAGE_CHECKER) bin/BOOT.BIN bin/KERNEL.BIN disk_img/dimos.img disk_img/dimos.hdd disk_img/dimos.iso

clean:
	rm -rf bin disk_img
