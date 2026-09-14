#!/usr/bin/env bash
# Build the DimOS Vulkan tools.
#
# Two host programs, both linking against the Khronos Vulkan loader:
#
#   dimos-vkviewer   displays a finished frame from the normal kernel
#   dimos-vkraster   rasterizes a command list from the kernel built
#                    without src/kernel/gfx.c, so the GPU decides every
#                    pixel
#
# Nothing here touches the kernel -- src/ builds exactly as it did.
#
# Environment:
#   VULKAN_SDK   prefix holding include/vulkan and lib/libvulkan.so
#                (defaults to /usr, i.e. a distro libvulkan-dev)
#   GLSLANG      the GLSL -> SPIR-V compiler
#                (defaults to whichever of glslang / glslangValidator is
#                 on PATH)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$REPO/bin"

VULKAN_SDK="${VULKAN_SDK:-/usr}"
CC="${CC:-gcc}"

if [[ -z "${GLSLANG:-}" ]]; then
    if command -v glslang >/dev/null 2>&1; then
        GLSLANG=glslang
    elif command -v glslangValidator >/dev/null 2>&1; then
        GLSLANG=glslangValidator
    else
        echo "build.sh: no GLSL compiler found; set GLSLANG=/path/to/glslang" >&2
        exit 1
    fi
fi

if [[ ! -f "$VULKAN_SDK/include/vulkan/vulkan.h" ]]; then
    echo "build.sh: no Vulkan headers under $VULKAN_SDK; set VULKAN_SDK" >&2
    exit 1
fi

mkdir -p "$OUT"

echo "Compiling shaders with $GLSLANG"
"$GLSLANG" -V --target-env vulkan1.0 "$HERE/shaders/dimos.vert" \
    -o "$HERE/shaders/dimos.vert.spv" >/dev/null
"$GLSLANG" -V --target-env vulkan1.0 "$HERE/shaders/dimos.frag" \
    -o "$HERE/shaders/dimos.frag.spv" >/dev/null
"$GLSLANG" -V --target-env vulkan1.0 "$HERE/shaders/raster.comp" \
    -o "$HERE/shaders/raster.comp.spv" >/dev/null
echo "  dimos.vert.spv, dimos.frag.spv, raster.comp.spv"

echo "Compiling the viewer"
"$CC" -std=c11 -O2 -Wall -Wextra \
    -I "$VULKAN_SDK/include" \
    "$HERE/vkviewer.c" \
    -L "$VULKAN_SDK/lib" -lvulkan -lm \
    -Wl,-rpath,"$VULKAN_SDK/lib" \
    -o "$OUT/dimos-vkviewer"
echo "  $OUT/dimos-vkviewer"

echo "Compiling the rasterizer"
"$CC" -std=c11 -O2 -Wall -Wextra \
    -I "$VULKAN_SDK/include" -I "$HERE" \
    "$HERE/vkraster.c" \
    -L "$VULKAN_SDK/lib" -lvulkan -lm \
    -Wl,-rpath,"$VULKAN_SDK/lib" \
    -o "$OUT/dimos-vkraster"
echo "  $OUT/dimos-vkraster"
