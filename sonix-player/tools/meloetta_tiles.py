#!/usr/bin/env python3
"""The Meloetta fork's menu tiles (assets/icons/<name>.png, 140 px).

The stock tiles' style: a solid back shape and a see-through front shape in
one hue, and on the front shape a glyph tinted toward that hue and itself a
little see-through. Every glyph here is scaled into the same box and centred
on the front shape, so the tiles sit together.

    python3 tools/meloetta_tiles.py [preview.png]     (needs pillow)

then tools/svg_to_lvgl.py to rebuild icons.c.
"""
import math
import os
import sys

from PIL import Image, ImageDraw

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(REPO_ROOT, "assets", "icons")
OUT = 140
S = 4 * OUT  # drawn at 4x, shrunk at the end

BACK_BOX = (0.30, 0.08, 0.92, 0.70)
FRONT_BOX = (0.08, 0.30, 0.70, 0.92)
FRONT_CX, FRONT_CY = 0.39, 0.61
GLYPH_BOX = 0.34  # the glyph's longer side, as a share of the tile
FRONT_OPA = 185
GLYPH_OPA = 205  # the stock glyphs let the front shape show through a little
GLYPH_TINT = 0.15  # and lean toward its colour


def shape(kind, box):
    m = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(m)
    b = [v * S for v in box]
    if kind == "circle":
        d.ellipse(b, fill=255)
    else:
        d.rounded_rectangle(b, radius=S // 7, fill=255)
    return m


def gradient(top, bottom):
    g = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(g)
    for y in range(S):
        t = y / (S - 1)
        d.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    return g


def fit(mask):
    """Scale the glyph mask into GLYPH_BOX and centre it on the front shape."""
    bb = mask.getbbox()
    g = mask.crop(bb)
    k = GLYPH_BOX * S / max(g.size)
    g = g.resize((max(1, round(g.width * k)), max(1, round(g.height * k))), Image.LANCZOS)
    out = Image.new("L", (S, S), 0)
    out.paste(g, (round(FRONT_CX * S - g.width / 2), round(FRONT_CY * S - g.height / 2)))
    return out


def tile(name, kind, back_cols, front_cols, glyph):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    back = gradient(*back_cols)
    back.putalpha(shape(kind, BACK_BOX))
    img.alpha_composite(back)
    front = gradient(*front_cols)
    front.putalpha(shape(kind, FRONT_BOX).point(lambda v: v * FRONT_OPA // 255))
    img.alpha_composite(front)

    m = Image.new("L", (S, S), 0)
    glyph(ImageDraw.Draw(m))
    tint = tuple(round(255 + (c - 255) * GLYPH_TINT) for c in front_cols[0])
    g = Image.new("RGBA", (S, S), tint + (0,))
    g.putalpha(fit(m).point(lambda v: v * GLYPH_OPA // 255))
    img.alpha_composite(g)
    return img.resize((OUT, OUT), Image.LANCZOS)


# --- glyphs, drawn anywhere at any size: fit() places them -------------------
U = S / 100  # one percent of the tile


def rr(d, x0, y0, x1, y1, r, fill=255, **kw):
    d.rounded_rectangle((x0 * U, y0 * U, x1 * U, y1 * U), radius=int(r * U), fill=fill, **kw)


def el(d, cx, cy, rx, ry=None, fill=255):
    ry = rx if ry is None else ry
    d.ellipse(((cx - rx) * U, (cy - ry) * U, (cx + rx) * U, (cy + ry) * U), fill=fill)


def line(d, pts, w, fill=255):
    p = [(x * U, y * U) for x, y in pts]
    d.line(p, fill=fill, width=int(w * U), joint="curve")
    for x, y in (p[0], p[-1]):
        r = w * U / 2
        d.ellipse((x - r, y - r, x + r, y + r), fill=fill)


def apps(d):
    for r in range(2):
        for c in range(2):
            rr(d, 20 + c * 24, 20 + r * 24, 40 + c * 24, 40 + r * 24, 6)


def games(d):
    rr(d, 10, 30, 90, 70, 20)
    rr(d, 22, 47, 42, 53, 1.5, fill=0)
    rr(d, 29, 40, 35, 60, 1.5, fill=0)
    el(d, 66, 54, 5, fill=0)
    el(d, 77, 44, 5, fill=0)


def puzzles(d):
    filled = {(0, 0), (1, 1), (2, 2), (2, 0)}
    for r in range(3):
        for c in range(3):
            box = (10 + c * 28, 10 + r * 28, 33 + c * 28, 33 + r * 28)
            if (r, c) in filled:
                rr(d, *box, 5)
            else:
                d.rounded_rectangle([v * U for v in box], radius=int(5 * U), outline=255, width=int(3.2 * U), fill=None)


def buds(d):
    for cx, flip in ((32, -1), (68, 1)):
        el(d, cx, 38, 14)
        el(d, cx + flip * 13, 38, 7, 8)
        sx = cx - flip * 5
        rr(d, sx - 4, 38, sx + 4, 78, 4)


def notes(d):
    d.polygon([(25 * U, 10 * U), (62 * U, 10 * U), (78 * U, 26 * U), (78 * U, 90 * U), (25 * U, 90 * U)], fill=255)
    d.polygon([(62 * U, 10 * U), (62 * U, 26 * U), (78 * U, 26 * U)], fill=0)
    for y, x1 in ((40, 68), (54, 68), (68, 56)):
        rr(d, 35, y, x1, y + 6, 3, fill=0)


def home(d):
    d.polygon([(8 * U, 48 * U), (50 * U, 12 * U), (92 * U, 48 * U)], fill=255)
    d.rectangle((20 * U, 46 * U, 80 * U, 88 * U), fill=255)
    d.rectangle((43 * U, 62 * U, 57 * U, 88 * U), fill=0)


def moon(d):
    el(d, 50, 50, 40)
    el(d, 72, 38, 34, fill=0)


def pet(d):
    el(d, 50, 54, 32, 40)
    for x, y, r in ((40, 40, 7), (62, 58, 9), (42, 72, 5)):
        el(d, x, y, r, fill=0)


def clocks(d):
    el(d, 50, 50, 40)
    el(d, 50, 50, 31, fill=0)
    line(d, [(50, 28), (50, 50), (66, 60)], 7)


def weather(d):
    for i in range(8):
        a = math.radians(i * 45)
        line(d, [(36 + 22 * math.cos(a), 36 + 22 * math.sin(a)), (36 + 30 * math.cos(a), 36 + 30 * math.sin(a))], 5)
    el(d, 36, 36, 15)
    # the cloud, cut free of the sun by a dark rim
    for cx, cy, r in ((48, 66, 17), (66, 58, 20), (82, 68, 14)):
        el(d, cx, cy, r + 5, fill=0)
    rr(d, 31, 64, 96, 87, 11, fill=0)
    for cx, cy, r in ((48, 66, 17), (66, 58, 20), (82, 68, 14)):
        el(d, cx, cy, r)
    rr(d, 36, 68, 92, 82, 7)


def meditation(d):
    el(d, 50, 50, 40)
    el(d, 54, 46, 27, fill=0)


def mini_golf(d):
    line(d, [(48, 15), (48, 74)], 6)
    d.polygon([(51*U, 15*U), (83*U, 28*U), (51*U, 40*U)], fill=255)
    el(d, 48, 79, 30, 7)
    el(d, 16, 70, 7)


def fabularium(d):
    rr(d, 19, 10, 81, 90, 9)
    rr(d, 25, 16, 75, 84, 6, fill=0)
    d.polygon([(50*U, 29*U), (57*U, 44*U), (69*U, 50*U),
               (57*U, 56*U), (50*U, 71*U), (43*U, 56*U),
               (31*U, 50*U), (43*U, 44*U)], fill=255)


TILES = [
    ("apps", "rounded", ((80, 200, 160), (40, 160, 120)), ((150, 230, 200), (80, 200, 160)), apps),
    ("games", "circle", ((235, 90, 110), (200, 50, 80)), ((250, 160, 175), (235, 90, 110)), games),
    ("puzzles", "rounded", ((150, 110, 230), (110, 70, 200)), ((200, 170, 245), (150, 110, 230)), puzzles),
    ("buds", "circle", ((90, 170, 230), (50, 120, 200)), ((150, 215, 245), (90, 170, 230)), buds),
    ("notes", "rounded", ((250, 210, 75), (236, 165, 25)), ((252, 214, 96), (244, 184, 48)), notes),
    ("home", "rounded", ((245, 160, 70), (225, 120, 40)), ((250, 200, 130), (240, 160, 80)), home),
    ("moon", "circle", ((150, 120, 240), (110, 80, 210)), ((190, 170, 250), (150, 120, 235)), moon),
    ("pet", "rounded", ((150, 207, 72), (112, 175, 42)), ((185, 225, 125), (150, 205, 80)), pet),
    ("clocks", "rounded", ((250, 178, 74), (234, 142, 42)), ((250, 200, 125), (244, 168, 78)), clocks),
    ("weather", "rounded", ((92, 172, 244), (58, 136, 224)), ((140, 195, 248), (98, 166, 240)), weather),
    ("meditation", "circle", ((240, 110, 155), (212, 72, 122)), ((245, 170, 198), (230, 125, 165)), meditation),
    ("mini_golf", "circle", ((80, 175, 115), (40, 125, 80)), ((155, 215, 165), (85, 175, 120)), mini_golf),
    ("fabularium", "rounded", ((170, 125, 70), (125, 80, 40)), ((230, 195, 130), (190, 145, 85)), fabularium),
]


def main():
    out = []
    for name, *spec in TILES:
        im = tile(name, *spec)
        im.save(os.path.join(ICON_DIR, name + ".png"))
        out.append(im)
    if len(sys.argv) > 1:
        sheet = Image.new("RGBA", (6 * 150 + 10, math.ceil(len(out)/6) * 150 + 10), (56, 58, 52, 255))
        for i, im in enumerate(out):
            sheet.alpha_composite(im, (10 + (i % 6) * 150, 10 + (i // 6) * 150))
        sheet.save(sys.argv[1])


if __name__ == "__main__":
    main()
