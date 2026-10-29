"""Home-screen icons for Kobalt on the PocketBook (same format as the KOReader ones from
jjrrw174's guide: 8-bit BMP, 106 x 110 px on the Era Lite).

  desktop_app_kobalt.bmp    normal: Kobalt's bars glyph in black on white
  desktop_app_kobalt_f.bmp  while tapped: the glyph white on a black disc

Usage: python make-icons.py <output dir>
"""
import os
import sys
from PIL import Image, ImageDraw

W, H = 106, 110
BAR_H, PITCH = 5, 9
BARS = 6
X0, X1 = 32, 81          # full bars
LAST_X1 = X0 + 28        # the shorter last bar
Y0 = (H - (BARS - 1) * PITCH - BAR_H) // 2


def glyph(draw, color):
    for i in range(BARS):
        y = Y0 + i * PITCH
        x1 = LAST_X1 if i == BARS - 1 else X1
        draw.rectangle([X0, y, x1, y + BAR_H - 1], fill=color)


def save(image, path):
    # 8-bit palette BMP, like the firmware's own icons
    image.convert("L").convert("P", palette=Image.ADAPTIVE, colors=256).save(path, format="BMP")


def main(out):
    os.makedirs(out, exist_ok=True)
    normal = Image.new("L", (W, H), 255)
    glyph(ImageDraw.Draw(normal), 0)
    save(normal, os.path.join(out, "desktop_app_kobalt.bmp"))

    focused = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(focused)
    r = 50
    cx, cy = W // 2, H // 2
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=0)
    glyph(d, 255)
    save(focused, os.path.join(out, "desktop_app_kobalt_f.bmp"))
    print("icons written to", out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
