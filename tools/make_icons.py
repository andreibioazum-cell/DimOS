#!/usr/bin/env python3
"""Draw the DimOS 14 phone-edition icons.

The web build ships four PNGs generated here, so the repository does not
have to carry binary blobs nobody can rebuild:

  web/icons/icon-192.png        launcher icon
  web/icons/icon-512.png        launcher icon, high density
  web/icons/maskable-512.png    Android adaptive icon (safe zone padded)
  web/icons/apple-touch-icon.png  iOS home-screen icon
  web/icons/favicon-32.png      browser tab

Run from the repository root:

    python3 tools/make_icons.py
"""

from __future__ import annotations

import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "web", "icons")

TOP = (90, 200, 250)      # #5ac8fa
MID = (10, 132, 255)      # #0a84ff
BOTTOM = (175, 82, 222)   # #af52de

FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
)


def font(size: int) -> ImageFont.FreeTypeFont:
    for candidate in FONT_CANDIDATES:
        if os.path.exists(candidate):
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def gradient(size: int) -> Image.Image:
    """A soft diagonal blue -> violet wash."""
    image = Image.new("RGB", (size, size))
    pixels = image.load()
    for y in range(size):
        for x in range(size):
            t = (x * 0.45 + y * 0.55) / max(1, size - 1)
            if t < 0.5:
                k = t / 0.5
                color = tuple(round(TOP[i] + (MID[i] - TOP[i]) * k) for i in range(3))
            else:
                k = (t - 0.5) / 0.5
                color = tuple(round(MID[i] + (BOTTOM[i] - MID[i]) * k) for i in range(3))
            pixels[x, y] = color
    return image


def rounded_mask(size: int, radius_ratio: float) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(mask)
    radius = round(size * radius_ratio)
    draw.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return mask


def draw_letter(image: Image.Image, scale: float = 0.56) -> None:
    draw = ImageDraw.Draw(image)
    size = image.width
    face = font(round(size * scale))
    text = "D"
    left, top, right, bottom = draw.textbbox((0, 0), text, font=face)
    x = (size - (right - left)) / 2 - left
    y = (size - (bottom - top)) / 2 - top - size * 0.025
    draw.text((x, y), text, font=face, fill=(255, 255, 255, 255))


def icon(size: int, radius_ratio: float, letter_scale: float = 0.56) -> Image.Image:
    base = gradient(size).convert("RGBA")
    draw_letter(base, letter_scale)
    base.putalpha(rounded_mask(size, radius_ratio))
    return base


def maskable(size: int) -> Image.Image:
    """Full-bleed art; Android crops it to its own shape."""
    base = gradient(size).convert("RGBA")
    # Inset the letter so it survives the adaptive-icon crop.
    draw_letter(base, 0.44)
    return base


def main() -> None:
    os.makedirs(OUT, exist_ok=True)

    icon(512, 0.225).save(os.path.join(OUT, "icon-512.png"))
    icon(192, 0.225).save(os.path.join(OUT, "icon-192.png"))
    icon(180, 0.225).save(os.path.join(OUT, "apple-touch-icon.png"))
    icon(32, 0.28, 0.62).save(os.path.join(OUT, "favicon-32.png"))
    maskable(512).save(os.path.join(OUT, "maskable-512.png"))

    print("wrote icons to", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
