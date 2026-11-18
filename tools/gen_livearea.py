#!/usr/bin/env python3
"""
Generate the LiveArea assets for Battleship (OLED radar style).

Outputs (8-bit palettised PNGs, as the Vita LiveArea requires):
  sce_sys/icon0.png                       128x128
  sce_sys/pic0.png                        960x544
  sce_sys/livearea/contents/bg.png        840x500
  sce_sys/livearea/contents/startup.png   280x158

Usage:  python3 tools/gen_livearea.py [path/to/bold_font.ttf]
Requires Pillow (pip install pillow).
"""
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CYAN, CORAL, AMBER, SLATE = (0, 240, 255), (255, 59, 92), (255, 159, 28), (26, 38, 57)

FONT_CANDIDATES = [
    sys.argv[1] if len(sys.argv) > 1 else "",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
]


def font(size):
    for p in FONT_CANDIDATES:
        if p and os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def radar(img, cx, cy, r, sweep_deg=-35, rings=4, grid=True, glow=True):
    w, h = img.size
    layer = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(layer)
    if grid:
        step = max(8, r // 6)
        for x in range(cx % step, w, step):
            d.line([(x, 0), (x, h)], fill=SLATE)
        for y in range(cy % step, h, step):
            d.line([(0, y), (w, y)], fill=SLATE)
    # sweep wedge with fading trail
    for i in range(60):
        a0 = math.radians(sweep_deg - i * 1.2)
        a1 = math.radians(sweep_deg - (i + 1) * 1.2)
        k = (1 - i / 60) ** 1.6
        col = tuple(int(c * 0.45 * k) for c in CYAN)
        d.polygon([(cx, cy), (cx + r * math.cos(a0), cy + r * math.sin(a0)),
                   (cx + r * math.cos(a1), cy + r * math.sin(a1))], fill=col)
    for i in range(1, rings + 1):
        rr = r * i / rings
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=(0, 90, 100), width=max(1, r // 90))
    d.line([(cx - r, cy), (cx + r, cy)], fill=(0, 70, 80))
    d.line([(cx, cy - r), (cx, cy + r)], fill=(0, 70, 80))
    a = math.radians(sweep_deg)
    d.line([(cx, cy), (cx + r * math.cos(a), cy + r * math.sin(a))], fill=CYAN, width=max(2, r // 60))
    # blips: two hits + a miss
    for (bx, by, col, s) in [(0.42, -0.30, CORAL, 0.07), (0.18, 0.46, CORAL, 0.055), (-0.5, 0.2, (90, 120, 160), 0.035)]:
        px, py, rad = cx + bx * r, cy + by * r, max(2, s * r)
        d.ellipse([px - rad, py - rad, px + rad, py + rad], fill=col)
    if glow:
        blur = layer.filter(ImageFilter.GaussianBlur(max(2, r // 40)))
        layer = Image.blend(layer, Image.eval(blur, lambda v: min(255, v * 2)), 0.35)
    img.paste(Image.composite(layer, img, Image.new("L", (w, h), 255)))


def ship(d, x, y, cells, cell, col, horiz=True):
    for i in range(cells):
        cx, cy = (x + i * cell, y) if horiz else (x, y + i * cell)
        d.rectangle([cx + 2, cy + 2, cx + cell - 3, cy + cell - 3], outline=col, width=2)


def glow_text(img, xy, text, f, col, anchor="mm", spread=6):
    w, h = img.size
    t = Image.new("RGB", (w, h), (0, 0, 0))
    ImageDraw.Draw(t).text(xy, text, font=f, fill=col, anchor=anchor)
    g = t.filter(ImageFilter.GaussianBlur(spread))
    out = Image.eval(Image.merge("RGB", [Image.eval(c, lambda v: min(255, v * 2)) for c in g.split()]), lambda v: v)
    img.paste(Image.composite(out, img, out.convert("L").point(lambda v: min(255, v * 2))))
    ImageDraw.Draw(img).text(xy, text, font=f, fill=col, anchor=anchor)


def save_indexed(img, rel):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(path, optimize=True)
    print("wrote", rel)


def icon():
    img = Image.new("RGB", (128, 128), (0, 0, 0))
    radar(img, 64, 64, 58, rings=3, grid=False)
    d = ImageDraw.Draw(img)
    d.ellipse([4, 4, 123, 123], outline=CYAN, width=3)
    glow_text(img, (64, 108), "BATTLESHIP", font(15), CYAN, spread=3)
    save_indexed(img, "sce_sys/icon0.png")


def bg():
    img = Image.new("RGB", (840, 500), (0, 0, 0))
    radar(img, 640, 250, 230)
    d = ImageDraw.Draw(img)
    ship(d, 60, 360, 4, 30, CYAN)
    ship(d, 60, 400, 3, 30, CYAN)
    ship(d, 170, 400, 2, 30, CYAN)
    ship(d, 240, 360, 1, 30, CYAN)
    glow_text(img, (60, 150), "BATTLESHIP", font(64), CYAN, anchor="lm", spread=8)
    d.text((64, 200), "TACTICAL NAVAL COMBAT", font=font(22), fill=(110, 130, 160), anchor="lm")
    d.line([(64, 228), (380, 228)], fill=CYAN, width=1)
    d.text((64, 262), "1P vs CPU   |   AD-HOC 2P", font=font(18), fill=AMBER, anchor="lm")
    save_indexed(img, "sce_sys/livearea/contents/bg.png")


def startup():
    img = Image.new("RGB", (280, 158), (0, 0, 0))
    radar(img, 225, 79, 70, rings=3, grid=False)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 279, 157], outline=CYAN, width=2)
    glow_text(img, (18, 70), "BATTLESHIP", font(26), CYAN, anchor="lm", spread=4)
    d.text((20, 98), "START", font=font(16), fill=AMBER, anchor="lm")
    save_indexed(img, "sce_sys/livearea/contents/startup.png")


def pic0():
    img = Image.new("RGB", (960, 544), (0, 0, 0))
    radar(img, 480, 272, 250)
    glow_text(img, (480, 272), "BATTLESHIP", font(80), CYAN, spread=10)
    save_indexed(img, "sce_sys/pic0.png")


if __name__ == "__main__":
    icon(); bg(); startup(); pic0()
