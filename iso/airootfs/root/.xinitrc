#!/bin/sh
# DimOS session: X with nothing but the DimOS binary on it.
#
#  - no screensaver and no DPMS blanking: the game must stay on screen;
#  - software GL (llvmpipe): the image carries no hardware GL driver, which is
#    what the live system needs under an emulator as well as on real GPUs;
#  - openbox runs only as a session skeleton (it maps and focuses the window
#    and paints nothing of its own).
xset s off
xset -dpms
xset s noblank

export LIBGL_ALWAYS_SOFTWARE=1
export DIMOS_FULLSCREEN=1

exec openbox-session
