#!/usr/bin/env python3
"""Original Jeopardy module icon: cobalt board, gold double border, gold glyph.

No show assets are used; the mark is original artwork drawn here with Pillow.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
FONT = ROOT.parents[2] / "hr54-ui/assets/Overpass.ttf"
SIZE = 256
GOLD = (245, 197, 66, 255)
GOLD2 = (176, 138, 38, 255)

img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# cobalt radial background inside a rounded square
for y in range(SIZE):
    for x in range(SIZE):
        dx = (x - SIZE / 2) / SIZE
        dy = (y - SIZE / 2) / SIZE
        r = (dx * dx + dy * dy) ** 0.5
        t = min(1.0, r * 1.6)
        d.point((x, y), (int(10 + (22 - 10) * (1 - t)), int(24 + (63 - 24) * (1 - t)), int(74 + (176 - 74) * (1 - t)), 255))
# rounded mask
mask = Image.new("L", (SIZE, SIZE), 0)
ImageDraw.Draw(mask).rounded_rectangle((4, 4, SIZE - 4, SIZE - 4), radius=44, fill=255)
img.putalpha(mask)
d = ImageDraw.Draw(img)
# gold double border
d.rounded_rectangle((10, 10, SIZE - 10, SIZE - 10), radius=38, outline=GOLD, width=6)
d.rounded_rectangle((20, 20, SIZE - 20, SIZE - 20), radius=30, outline=GOLD2, width=3)
# gold "J!" glyph
font = ImageFont.truetype(str(FONT), 132)
text = "J!"
bbox = d.textbbox((0, 0), text, font=font)
w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
d.text(((SIZE - w) / 2 - bbox[0], (SIZE - h) / 2 - bbox[1] - 6), text, font=font, fill=GOLD)

img.save(ROOT / "icon.png")
print("icon:", ROOT / "icon.png", (ROOT / "icon.png").stat().st_size)
