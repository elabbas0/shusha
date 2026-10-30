#!/usr/bin/env python3
"""Draw the Shusha wallpaper artwork: the Xarıbülbül emblem - magenta
petals, the dark purple body and wing, the yellow-green nightingale, all
in thick gold rims.  Shapes follow the emblem's proportions; drawn
procedurally (no image files), rendered with 4x supersampling to a
transparent PNG.  The WM places it on a solid colour.

    python3 scripts/gen-wallpaper.py user/assets/xari-bulbul.png
"""
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

W, H, SS = 800, 880, 4
OX, OY = 20, 20              # design space (about 790x780) -> canvas
FONT = "/usr/share/fonts/truetype/liberation/LiberationSerif-Italic.ttf"
FONT_SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

MAGENTA = (233, 52, 246)
MAGENTA_DEEP = (176, 36, 226)
PURPLE = (90, 14, 111)
PURPLE_LIGHT = (122, 30, 146)
YELLOW = (255, 255, 86)
GREEN = (171, 239, 75)

# Outlines (design space), traced from the emblem's proportions.
TOP_PETAL = [(105, 58), (125, 50), (150, 60), (195, 105), (250, 165), (300, 230), (340, 290), (352, 330),
             (345, 370), (325, 392), (300, 386), (262, 370), (200, 314), (150, 250), (118, 186), (100, 132),
             (95, 88)]
LEFT_PETAL = [(325, 392), (282, 398), (220, 428), (150, 488), (90, 565), (52, 645), (36, 715), (32, 758),
              (70, 762), (140, 748), (210, 715), (265, 670), (300, 625), (325, 570), (340, 510), (345, 450)]
SMALL_PETAL = [(400, 612), (425, 604), (450, 640), (478, 700), (495, 745), (478, 762), (458, 752), (435, 700),
               (412, 650)]
BODY = [(498, 395), (508, 428), (524, 456), (575, 432), (640, 405), (700, 385), (745, 370), (758, 382),
        (742, 402), (700, 432), (655, 465), (618, 505), (612, 522), (645, 555), (672, 600), (688, 650),
        (700, 700), (712, 728), (705, 752), (680, 765), (620, 768), (565, 760), (520, 745), (495, 715),
        (478, 670), (455, 620), (432, 570), (440, 500), (462, 452), (478, 415)]
BIRD = [(338, 458), (340, 392), (358, 328), (392, 272), (428, 228), (466, 205), (512, 202), (542, 218),
        (562, 240), (578, 260), (572, 278), (548, 280), (512, 278), (492, 285), (490, 318), (500, 362),
        (500, 396), (472, 398), (425, 415), (380, 440)]
LOBE = [(466, 398), (480, 412), (478, 450), (462, 495), (440, 540), (428, 585), (412, 625), (385, 645),
        (345, 645), (305, 625), (278, 590), (266, 545), (272, 508), (298, 492), (330, 478), (352, 462),
        (392, 440), (430, 418), (450, 402)]


def P(x, y):
    return (round((x + OX) * SS), round((y + OY) * SS))


def smooth(points, n=10):
    """Closed Catmull-Rom spline through the outline points."""
    out, m = [], len(points)
    for i in range(m):
        p0, p1, p2, p3 = points[i - 1], points[i], points[(i + 1) % m], points[(i + 2) % m]
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * (2 * p1[j] + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2 +
                                    (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in (0, 1)))
    return out


def blank(mode="L"):
    return Image.new(mode, (W * SS, H * SS), 0 if mode == "L" else (0, 0, 0, 0))


def shape_mask(pts):
    m = blank()
    ImageDraw.Draw(m).polygon([P(*p) for p in pts], fill=255)
    return m


def vgradient(top_rgb, bottom_rgb, y0, y1):
    """Full-canvas vertical gradient between design rows y0 and y1."""
    g = Image.new("RGBA", (1, H * SS))
    a, b = (y0 + OY) * SS, (y1 + OY) * SS
    for y in range(H * SS):
        t = min(1, max(0, (y - a) / (b - a)))
        g.putpixel((0, y), tuple(int(top_rgb[i] + (bottom_rgb[i] - top_rgb[i]) * t) for i in range(3)) + (255,))
    return g.resize((W * SS, H * SS))


def fill(img, pts, color, shade=None):
    """Flat enamel with a soft inner shade (darker towards the rim)."""
    m = shape_mask(pts)
    src = color if isinstance(color, Image.Image) else Image.new("RGBA", img.size, color + (255,))
    img.paste(src, (0, 0), m)
    if shade:
        rim = ImageChops.subtract(m, m.filter(ImageFilter.GaussianBlur(14 * SS)).point(lambda v: min(255, v * 2)))
        img.paste(Image.new("RGBA", img.size, shade + (255,)), (0, 0), rim.point(lambda v: v * 3 // 5))
    return m


def streak(img, m, pts, color, width, blur):
    """A soft colour band inside a shape (enamel depth)."""
    s = blank()
    ImageDraw.Draw(s).line([P(*p) for p in pts], fill=255, width=round(width * SS), joint="curve")
    s = ImageChops.multiply(s.filter(ImageFilter.GaussianBlur(blur * SS)), m)
    img.paste(Image.new("RGBA", img.size, color + (255,)), (0, 0), s)


def gold(img, pts):
    """Thick polished gold rim: dark edge, gold body, bright core, glint."""
    path = [P(*p) for p in pts] + [P(*pts[0])]
    d = ImageDraw.Draw(img)
    for color, width in (((110, 74, 8), 17), ((214, 160, 34), 14), ((246, 204, 70), 10),
                         ((255, 228, 110), 6), ((255, 246, 190), 2.2)):
        d.line(path, fill=color + (255,), width=round(width * SS), joint="curve")


def glow(img, at, radius, color, alpha=200):
    g = blank("RGBA")
    x, y = P(*at)
    r = radius * SS
    ImageDraw.Draw(g).ellipse((x - r, y - r, x + r, y + r), fill=color + (alpha,))
    img.alpha_composite(g.filter(ImageFilter.GaussianBlur(r * 0.55)))


def glint(img, at, size):
    g = blank("RGBA")
    x, y = P(*at)
    s = size * SS
    d = ImageDraw.Draw(g)
    d.polygon([(x - s, y), (x, y - s * 0.12), (x + s, y), (x, y + s * 0.12)], fill=(255, 255, 255, 240))
    d.polygon([(x, y - s * 0.7), (x + s * 0.1, y), (x, y + s * 0.7), (x - s * 0.1, y)], fill=(255, 255, 255, 240))
    img.alpha_composite(g.filter(ImageFilter.GaussianBlur(SS * 0.7)))


def draw(path):
    img = blank("RGBA")
    top, left, small = smooth(TOP_PETAL), smooth(LEFT_PETAL), smooth(SMALL_PETAL)
    body, bird, lobe = smooth(BODY), smooth(BIRD), smooth(LOBE)

    m = fill(img, top, MAGENTA, shade=MAGENTA_DEEP)
    streak(img, m, [(140, 120), (200, 210), (270, 300), (320, 360)], MAGENTA_DEEP, 16, 7)
    gold(img, top)

    m = fill(img, left, MAGENTA, shade=MAGENTA_DEEP)
    streak(img, m, [(300, 430), (220, 500), (150, 590), (90, 690)], MAGENTA_DEEP, 20, 9)
    streak(img, m, [(200, 650), (120, 720)], (246, 140, 252), 10, 6)
    gold(img, left)

    fill(img, small, MAGENTA, shade=MAGENTA_DEEP)
    gold(img, small)

    m = fill(img, body, PURPLE, shade=(64, 8, 82))
    streak(img, m, [(560, 560), (620, 640), (650, 720)], PURPLE_LIGHT, 34, 22)
    streak(img, m, [(560, 450), (660, 410), (730, 385)], PURPLE_LIGHT, 10, 6)
    gold(img, body)

    m = fill(img, bird, vgradient(YELLOW, GREEN, 210, 440), shade=(140, 210, 60))
    streak(img, m, [(400, 300), (440, 250), (490, 225)], (255, 255, 170), 10, 7)
    gold(img, bird)

    m = fill(img, lobe, PURPLE, shade=(64, 8, 82))
    streak(img, m, [(320, 520), (360, 580), (400, 610)], PURPLE_LIGHT, 26, 16)
    gold(img, lobe)

    glow(img, (345, 455), 26, (255, 120, 255), 170)
    glint(img, (345, 455), 30)
    glow(img, (362, 505), 10, (255, 60, 120), 160)

    d = ImageDraw.Draw(img)
    cx = W * SS // 2
    d.text((cx, (H - 52) * SS), "Xarıbülbül", font=ImageFont.truetype(FONT, 36 * SS), fill=(226, 200, 236, 255), anchor="mm")
    d.text((cx, (H - 16) * SS), "Şuşa · Qarabağ", font=ImageFont.truetype(FONT_SANS, 15 * SS), fill=(150, 160, 176, 255), anchor="mm")

    img.resize((W, H), Image.LANCZOS).save(path, optimize=True)


if __name__ == "__main__":
    draw(sys.argv[1] if len(sys.argv) > 1 else "xari-bulbul.png")
