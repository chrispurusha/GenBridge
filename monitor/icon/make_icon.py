#!/usr/bin/env python3
"""Draws GenBridge Monitor's app icon (AppIcon-1024.png) in the siblings' manner - a rounded square of dark
brushed hardware, metallic lettering, and the app's job drawn as a control surface: two jack sockets joined by
a glowing patch cable, with a stereo pair of LED meters between them.

    python3 monitor/icon/make_icon.py          (needs Pillow)

do-monitor turns the PNG into the .icns; this only needs running again to change the artwork.
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 1024
HERE = os.path.dirname(os.path.abspath(__file__))
FONT_HEAVY = "/System/Library/Fonts/Supplemental/Arial Black.ttf"
FONT_COND = "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf"
BLUE = (70, 160, 255)


def rounded_mask(size, inset, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([inset, inset, size - inset, size - inset], radius, fill=255)
    return m


def vertical_gradient(size, top, bottom):
    g = Image.new("RGB", (1, size))
    for y in range(size):
        t = y / (size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return g.resize((size, size))


def brushed(img, amount=10, seed=3):
    random.seed(seed)
    noise = Image.new("L", (S, S))
    px = noise.load()
    for y in range(S):
        row = random.randint(-amount, amount)
        for x in range(0, S, 4):
            v = 128 + row + random.randint(-amount // 2, amount // 2)
            for k in range(4):
                if x + k < S:
                    px[x + k, y] = v
    noise = noise.filter(ImageFilter.GaussianBlur(radius=(6, 0.4)) if hasattr(ImageFilter, "BoxBlur") else 1)
    return Image.blend(img, Image.merge("RGB", (noise, noise, noise)), 0.07)


def metal_text(base, xy, text, font, top, bottom, shadow=True, glow=None):
    w, h = base.size
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).text(xy, text, font=font, fill=255, anchor="mm")
    if glow is not None:
        g = Image.new("RGB", (w, h), glow)
        gm = mask.filter(ImageFilter.GaussianBlur(26))
        base.paste(g, (0, 0), gm.point(lambda v: int(v * 0.9)))
    if shadow:
        sh = mask.filter(ImageFilter.GaussianBlur(8)).point(lambda v: int(v * 0.8))
        base.paste((0, 0, 0), (6, 10), sh)
    bbox = mask.getbbox()
    grad = Image.new("RGB", (w, h))
    gd = ImageDraw.Draw(grad)
    y0, y1 = bbox[1], bbox[3]
    for y in range(y0, y1 + 1):
        t = (y - y0) / max(1, (y1 - y0))
        # a bright band a third of the way down, as polished lettering catches the light
        k = 1.0 - abs(t - 0.32) * 1.6
        k = max(0.0, min(1.0, k))
        c = tuple(int(bottom[i] + (top[i] - bottom[i]) * k) for i in range(3))
        gd.line([(0, y), (w, y)], fill=c)
    base.paste(grad, (0, 0), mask)
    edge = mask.filter(ImageFilter.FIND_EDGES).point(lambda v: int(v * 0.35))
    base.paste((255, 255, 255), (0, -2), edge)


def socket(d, cx, cy, r):
    # nut, ring, hole - a quarter-inch jack seen face on
    d.ellipse([cx - r * 1.35, cy - r * 1.35, cx + r * 1.35, cy + r * 1.35], fill=(18, 20, 24))
    for i in range(10, 0, -1):
        t = i / 10
        c = int(70 + 120 * (1 - t))
        rr = r * (0.75 + 0.5 * t)
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(c, c, c + 6))
    d.ellipse([cx - r * 0.72, cy - r * 0.72, cx + r * 0.72, cy + r * 0.72], fill=(30, 32, 36))
    d.ellipse([cx - r * 0.45, cy - r * 0.45, cx + r * 0.45, cy + r * 0.45], fill=(6, 7, 9))


def cable(img, p0, p1, sag):
    w, h = img.size
    pts = []
    for i in range(101):
        t = i / 100
        x = p0[0] + (p1[0] - p0[0]) * t
        y = p0[1] + (p1[1] - p0[1]) * t + sag * math.sin(math.pi * t)
        pts.append((x, y))
    glow = Image.new("L", (w, h), 0)
    ImageDraw.Draw(glow).line(pts, fill=255, width=46, joint="curve")
    img.paste(BLUE, (0, 0), glow.filter(ImageFilter.GaussianBlur(22)).point(lambda v: int(v * 0.85)))
    d = ImageDraw.Draw(img)
    d.line(pts, fill=(14, 40, 80), width=34, joint="curve")
    d.line(pts, fill=(60, 150, 255), width=22, joint="curve")
    d.line([(x, y - 5) for x, y in pts], fill=(185, 225, 255), width=6, joint="curve")


def meter(d, x, y, w, h, lit):
    seg = 12
    gap = 7
    sh = (h - gap * (seg - 1)) / seg
    for i in range(seg):
        top = y + h - (i + 1) * sh - i * gap
        on = i < lit
        if i >= 10:
            col = (255, 70, 55) if on else (60, 20, 18)
        elif i >= 8:
            col = (255, 200, 50) if on else (60, 50, 16)
        else:
            col = (70, 230, 110) if on else (18, 50, 26)
        d.rounded_rectangle([x, top, x + w, top + sh], 4, fill=col)


def main():
    inset = 44
    radius = 200

    body = vertical_gradient(S, (72, 78, 88), (22, 25, 30))
    body = brushed(body)

    # the lower panel, recessed - where the sockets and meters sit
    d = ImageDraw.Draw(body)
    panel = [inset + 70, 540, S - inset - 70, S - inset - 70]
    d.rounded_rectangle([panel[0] - 4, panel[1] - 4, panel[2] + 4, panel[3] + 4], 46, fill=(90, 96, 106))
    d.rounded_rectangle(panel, 42, fill=(14, 16, 20))
    inner = Image.new("L", (S, S), 0)
    ImageDraw.Draw(inner).rounded_rectangle(panel, 42, fill=255)
    shade = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shade).rounded_rectangle([panel[0], panel[1], panel[2], panel[1] + 40], 42, fill=140)
    body.paste((0, 0, 0), (0, 0), Image.composite(shade.filter(ImageFilter.GaussianBlur(14)), Image.new("L", (S, S), 0), inner))

    metal_text(body, (S / 2, 252), "GEN", ImageFont.truetype(FONT_HEAVY, 210), (250, 252, 255), (120, 128, 140))
    metal_text(body, (S / 2, 430), "MONITOR", ImageFont.truetype(FONT_COND, 188), (215, 238, 255), (40, 120, 230),
               glow=BLUE)

    d = ImageDraw.Draw(body)
    lx, rx, sy = panel[0] + 110, panel[2] - 110, panel[1] + 120
    meter(d, S / 2 - 58, panel[1] + 36, 46, 196, 9)
    meter(d, S / 2 + 12, panel[1] + 36, 46, 196, 8)
    cable(body, (lx, sy), (rx, sy), 165)
    d = ImageDraw.Draw(body)
    socket(d, lx, sy, 56)
    socket(d, rx, sy, 56)

    # power LED, top right, as the EMU icon has
    led = Image.new("L", (S, S), 0)
    ImageDraw.Draw(led).ellipse([S - inset - 150, inset + 92, S - inset - 120, inset + 122], fill=255)
    body.paste(BLUE, (0, 0), led.filter(ImageFilter.GaussianBlur(14)))
    body.paste((190, 230, 255), (0, 0), led)

    # bevel: a light top edge and a dark bottom one, inside the rounded square
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    mask = rounded_mask(S, inset, radius)
    rim = Image.new("L", (S, S), 0)
    ImageDraw.Draw(rim).rounded_rectangle([inset, inset, S - inset, S - inset], radius, outline=255, width=10)
    top_light = vertical_gradient(S, (255, 255, 255), (40, 40, 40))
    body.paste(top_light, (0, 0), rim.point(lambda v: int(v * 0.35)))
    out.paste(body, (0, 0), mask)

    # drop shadow under the tile, as macOS icons carry
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sm = rounded_mask(S, inset, radius).filter(ImageFilter.GaussianBlur(18))
    shadow.paste((0, 0, 0, 255), (0, 14), sm.point(lambda v: int(v * 0.45)))
    final = Image.alpha_composite(shadow, out)
    final.save(os.path.join(HERE, "AppIcon-1024.png"))
    print("wrote", os.path.join(HERE, "AppIcon-1024.png"))


if __name__ == "__main__":
    main()
