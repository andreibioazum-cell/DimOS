CXX ?= g++
CXXFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror
IMAGE_CHECKER := bin/dimos-image-check

.PHONY: all iso tools verify clean

all: iso

iso: tools
	./build-linux.sh

tools: $(IMAGE_CHECKER)

$(IMAGE_CHECKER): tools/image_inspector.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -std=c++17 $(CXXFLAGS) $(WARNINGS) $< -o $@

verify: tools
	$(IMAGE_CHECKER) bin/BOOT.BIN bin/KERNEL.BIN disk_img/dimos.img disk_img/dimos.iso

clean:
	rm -rf bin disk_img
