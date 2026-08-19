CXX ?= g++
CXXFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror
IMAGE_CHECKER := bin/dimos-image-check

PYTHON ?= python3

.PHONY: all iso tools verify verify-embedded-image clean

all: iso

iso: tools
	./build-linux.sh

tools: $(IMAGE_CHECKER)

$(IMAGE_CHECKER): tools/image_inspector.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -std=c++17 $(CXXFLAGS) $(WARNINGS) $< -o $@

verify: tools
	$(IMAGE_CHECKER) bin/BOOT.BIN bin/KERNEL.BIN disk_img/dimos.img disk_img/dimos.iso

# index.html boots the payload embedded in web/dimos-image.js, so that payload
# needs its own check: a stale one silently boots an outdated kernel.
verify-embedded-image:
	$(PYTHON) tools/check_embedded_image.py

clean:
	rm -rf bin disk_img
