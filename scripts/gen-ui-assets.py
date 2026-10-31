#!/usr/bin/env python3
"""Generate user/lib/ui_assets.h: anti-aliased glyph atlases, icons and
the mouse cursor for the Shusha UI.

Needs Pillow and the DejaVu fonts (Bitstream Vera license, see NOTICE).
The output is committed, so a normal build does not need Pillow; rerun
this only when changing fonts or artwork:

    python3 scripts/gen-ui-assets.py > user/lib/ui_assets.h
"""
import math
from PIL import Image, ImageDraw, ImageFilter, ImageFont

DEJAVU = "/usr/share/fonts/truetype/dejavu/"
LINE_H = 16   # every face uses a 16 px line box ...
BASE = 12     # ... with the baseline 12 px below its top

# ASCII plus Azerbaijani letters and a little typography.
CHARSET = list(range(32, 127)) + [ord(c) for c in "ƏəŞşĞğıİÇçÖöÜü·—…×©"]

FACES = [
    ("sans", DEJAVU + "DejaVuSans.ttf", 13),
    ("bold", DEJAVU + "DejaVuSans-Bold.ttf", 13),
    ("mono", DEJAVU + "DejaVuSansMono.ttf", 13),
]

out = []
emit = out.append


def c_bytes(name, data, kind="uint8_t"):
    emit("static const %s %s[%d] = {" % (kind, name, len(data)))
    fmt = "0x%02x" if kind == "uint8_t" else "0x%08x"
    per = 24 if kind == "uint8_t" else 8
    for i in range(0, len(data), per):
        emit("    " + ",".join(fmt % v for v in data[i:i + per]) + ",")
    emit("};")


def gen_face(name, path, size):
    font = ImageFont.truetype(path, size)
    glyphs, bitmap = [], []
    for cp in CHARSET:
        ch = chr(cp)
        adv = int(round(font.getlength(ch)))
        img = Image.new("L", (size * 4, size * 4), 0)
        d = ImageDraw.Draw(img)
        ox, oy = size, size
        d.text((ox, oy + BASE), ch, font=font, fill=255, anchor="ls")
        box = img.getbbox()
        if not box:
            glyphs.append((cp, adv, 0, 0, 0, 0, len(bitmap)))
            continue
        crop = img.crop(box)
        w, h = crop.size
        glyphs.append((cp, adv, box[0] - ox, box[1] - oy, w, h, len(bitmap)))
        bitmap.extend(crop.tobytes())
    c_bytes("font_%s_bits" % name, bitmap)
    emit("static const glyph_t font_%s_glyphs[%d] = {" % (name, len(glyphs)))
    for g in glyphs:
        emit("    { 0x%04x, %d, %d, %d, %d, %d, %d }," % g)
    emit("};")


def premul(img):
    """RGBA image -> list of premultiplied 0xAARRGGBB words."""
    words = []
    raw = img.tobytes()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
        words.append((a << 24) | ((r * a // 255) << 16) | ((g * a // 255) << 8) | (b * a // 255))
    return words


def rrect(d, box, r, fill, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def icon(fn, size=48, ss=4):
    img = Image.new("RGBA", (size * ss, size * ss), (0, 0, 0, 0))
    fn(ImageDraw.Draw(img), ss)
    return img.resize((size, size), Image.LANCZOS)


def draw_folder(d, s):
    rrect(d, (4 * s, 8 * s, 22 * s, 18 * s), 3 * s, (201, 146, 52, 255))
    rrect(d, (4 * s, 12 * s, 44 * s, 40 * s), 4 * s, (224, 168, 66, 255))
    rrect(d, (4 * s, 16 * s, 44 * s, 41 * s), 4 * s, (242, 192, 88, 255))
    d.rectangle((6 * s, 17 * s, 42 * s, 18 * s), fill=(250, 214, 130, 255))


def draw_file(d, s):
    d.polygon([(10 * s, 4 * s), (30 * s, 4 * s), (38 * s, 12 * s), (38 * s, 44 * s), (10 * s, 44 * s)], fill=(226, 232, 240, 255))
    d.polygon([(30 * s, 4 * s), (38 * s, 12 * s), (30 * s, 12 * s)], fill=(170, 182, 198, 255))
    for i in range(4):
        y = (19 + i * 6) * s
        d.rounded_rectangle((15 * s, y, (33 - (8 if i == 3 else 0)) * s, y + 2 * s), radius=s, fill=(140, 152, 170, 255))


def draw_app(d, s):
    rrect(d, (5 * s, 5 * s, 43 * s, 43 * s), 10 * s, (47, 148, 136, 255))
    rrect(d, (5 * s, 5 * s, 43 * s, 37 * s), 10 * s, (63, 182, 168, 255))
    for (x, y) in ((14, 14), (26, 14), (14, 26), (26, 26)):
        rrect(d, (x * s, y * s, (x + 8) * s, (y + 8) * s), 2 * s, (233, 251, 248, 255))


def draw_terminal(d, s):
    rrect(d, (4 * s, 6 * s, 44 * s, 42 * s), 8 * s, (58, 70, 86, 255))
    rrect(d, (5 * s, 7 * s, 43 * s, 41 * s), 7 * s, (20, 27, 37, 255))
    w = int(3.2 * s)
    d.line([(12 * s, 17 * s), (19 * s, 23 * s), (12 * s, 29 * s)], fill=(95, 212, 197, 255), width=w, joint="curve")
    d.line([(22 * s, 31 * s), (33 * s, 31 * s)], fill=(230, 237, 245, 255), width=w)


def draw_power(d, s):
    w = int(1.8 * s)
    d.arc((2 * s, 3 * s, 14 * s, 15 * s), start=-60, end=240, fill=(214, 222, 232, 255), width=w)
    d.line([(8 * s, 1 * s), (8 * s, 8 * s)], fill=(214, 222, 232, 255), width=w)


def draw_workspace(d, s):
    # Three machines joined into one space, in the wallpaper's colours.
    rrect(d, (5 * s, 5 * s, 43 * s, 43 * s), 10 * s, (74, 18, 96, 255))
    rrect(d, (5 * s, 5 * s, 43 * s, 37 * s), 10 * s, (112, 30, 140, 255))
    nodes = [(24, 14), (14, 32), (34, 32)]
    w = int(2.4 * s)
    for i in range(3):
        a, b = nodes[i], nodes[(i + 1) % 3]
        d.line([(a[0] * s, a[1] * s), (b[0] * s, b[1] * s)], fill=(233, 52, 246, 255), width=w)
    for i, (x, y) in enumerate(nodes):
        r = 5.2 if i == 0 else 4.4
        d.ellipse(((x - r) * s, (y - r) * s, (x + r) * s, (y + r) * s), fill=(255, 214, 90, 255) if i == 0 else (240, 244, 250, 255))


def draw_lock(d, s):
    w = int(1.7 * s)
    d.arc((4 * s, 1 * s, 12 * s, 11 * s), start=180, end=360, fill=(214, 222, 232, 255), width=w)
    d.line([(4.85 * s, 6 * s), (4.85 * s, 8 * s)], fill=(214, 222, 232, 255), width=w)
    d.line([(11.15 * s, 6 * s), (11.15 * s, 8 * s)], fill=(214, 222, 232, 255), width=w)
    rrect(d, (2.5 * s, 7 * s, 13.5 * s, 15 * s), 2 * s, (230, 180, 80, 255))
    d.ellipse((7 * s, 9.6 * s, 9 * s, 11.6 * s), fill=(90, 60, 20, 255))


def draw_code(d, s):
    # An editor: dark page, angle brackets and a slash.
    rrect(d, (5 * s, 5 * s, 43 * s, 43 * s), 10 * s, (20, 90, 160, 255))
    rrect(d, (5 * s, 5 * s, 43 * s, 37 * s), 10 * s, (37, 124, 204, 255))
    w = int(3.2 * s)
    col = (240, 248, 255, 255)
    d.line([(18 * s, 16 * s), (11 * s, 24 * s), (18 * s, 32 * s)], fill=col, width=w, joint="curve")
    d.line([(30 * s, 16 * s), (37 * s, 24 * s), (30 * s, 32 * s)], fill=col, width=w, joint="curve")
    d.line([(26.5 * s, 13 * s), (21.5 * s, 35 * s)], fill=(255, 214, 90, 255), width=w)


def gen_cursor():
    ss, W, H = 8, 24, 28
    pts = [(1, 1), (1, 18), (5.2, 14.2), (8.2, 21), (10.6, 20), (7.7, 13.4), (13.4, 13.4)]
    pts = [((x + 1) * ss, (y + 1) * ss) for x, y in pts]
    shadow = Image.new("L", (W * ss, H * ss), 0)
    ImageDraw.Draw(shadow).polygon([(x + ss, y + 2 * ss) for x, y in pts], fill=110)
    shadow = shadow.filter(ImageFilter.GaussianBlur(1.6 * ss))
    img = Image.new("RGBA", (W * ss, H * ss), (0, 0, 0, 0))
    img.putalpha(shadow)
    d = ImageDraw.Draw(img)
    d.polygon(pts, fill=(255, 255, 255, 255), outline=(20, 20, 24, 255), width=int(1.3 * ss))
    return img.resize((W, H), Image.LANCZOS)


emit("/* Generated by scripts/gen-ui-assets.py - do not edit. */")
emit("#ifndef SHUSHA_UI_ASSETS_H")
emit("#define SHUSHA_UI_ASSETS_H")
emit("#include <stdint.h>")
emit("typedef struct { uint16_t cp; uint8_t adv; int8_t xo, yo; uint8_t w, h; uint32_t off; } glyph_t;")
emit("#define FONT_LINE_H %d" % LINE_H)
emit("#define FONT_GLYPHS %d" % len(CHARSET))
for face in FACES:
    gen_face(*face)

for name, fn, size in (("folder", draw_folder, 48), ("file", draw_file, 48), ("app", draw_app, 48),
                       ("terminal", draw_terminal, 48), ("power", draw_power, 16),
                       ("workspace", draw_workspace, 48), ("lock", draw_lock, 16),
                       ("code", draw_code, 48)):
    emit("#define ICON_%s_SIZE %d" % (name.upper(), size))
    c_bytes("icon_%s" % name, premul(icon(fn, size)), "uint32_t")

cur = gen_cursor()
emit("#define CURSOR_W %d" % cur.size[0])
emit("#define CURSOR_H %d" % cur.size[1])
c_bytes("cursor_img", premul(cur), "uint32_t")
emit("#endif")
print("\n".join(out))
