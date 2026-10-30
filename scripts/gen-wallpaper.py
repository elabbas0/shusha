#!/usr/bin/env python3
"""Draw the Shusha wallpaper artwork: the Xarıbülbül emblem - stylised
flower petals (magenta and dark purple), the green nightingale perched on
the flower, and gold enamel outlines.  Original drawing made procedurally
(no external images), rendered with 4x supersampling to a transparent
PNG.  The WM places it on a solid colour.

    python3 scripts/gen-wallpaper.py user/assets/xari-bulbul.png
"""
import math
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

W, H, SS = 800, 900, 4      # canvas; the emblem is drawn in a 1000x1000 design space
K = 0.78                    # design units -> canvas pixels
OX, OY = 10, -30            # design-space offset
FONT = "/usr/share/fonts/truetype/liberation/LiberationSerif-Italic.ttf"
FONT_SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

GOLD_DARK, GOLD, GOLD_HI = (150, 104, 22, 255), (222, 172, 58, 255), (255, 236, 160, 255)


def P(x, y):
    """Design point -> supersampled canvas point."""
    return (round((x * K + OX) * SS), round((y * K + OY) * SS))


def mask():
    return Image.new("L", (W * SS, H * SS), 0)


def layer():
    return Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))


def catmull(points, n=16):
    """Closed Catmull-Rom spline through design points."""
    out = []
    m = len(points)
    for i in range(m):
        p0, p1, p2, p3 = points[i - 1], points[i], points[(i + 1) % m], points[(i + 2) % m]
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            x = 0.5 * (2 * p1[0] + (-p0[0] + p2[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * (2 * p1[1] + (-p0[1] + p2[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            out.append((x, y))
    return out


def leaf(base, tip, width, bulge=0.45, sharp=0.9, n=60):
    """Pointed petal from base to tip, in design space."""
    dx, dy = tip[0] - base[0], tip[1] - base[1]
    length = math.hypot(dx, dy)
    ux, uy = dx / length, dy / length
    px, py = -uy, ux
    left, right = [], []
    for i in range(n + 1):
        t = i / n
        if t < bulge:
            w = math.sin(t / bulge * math.pi / 2) ** 0.7
        else:
            w = math.cos((t - bulge) / (1 - bulge) * math.pi / 2) ** sharp
        hw = width / 2 * w
        cx, cy = base[0] + dx * t, base[1] + dy * t
        left.append((cx + px * hw, cy + py * hw))
        right.append((cx - px * hw, cy - py * hw))
    return left + right[::-1]


def shape_mask(pts):
    m = mask()
    ImageDraw.Draw(m).polygon([P(*p) for p in pts], fill=255)
    return m


def enamel(img, pts, dark, mid, light, hi_pts=None):
    """Glossy enamel fill: dark rim, lighter centre, soft highlight band."""
    m = shape_mask(pts)
    img.paste(Image.new("RGBA", img.size, dark), (0, 0), m)
    inner = m.filter(ImageFilter.GaussianBlur(28 * SS * K))
    inner = ImageChops.multiply(inner.point(lambda v: min(255, int(v * 1.6))), m)
    img.paste(Image.new("RGBA", img.size, mid), (0, 0), inner)
    if hi_pts:
        hm = shape_mask(hi_pts).filter(ImageFilter.GaussianBlur(9 * SS * K))
        img.paste(Image.new("RGBA", img.size, light), (0, 0), ImageChops.multiply(hm, m))
    return m


def gold_outline(img, pts, scale=1.0):
    """Bevelled gold rim along a closed outline."""
    path = [P(*p) for p in pts] + [P(*pts[0])]
    for color, width in ((GOLD_DARK, 15 * scale), (GOLD, 10 * scale), (GOLD_HI, 3 * scale)):
        ImageDraw.Draw(img).line(path, fill=color, width=round(width * SS * K), joint="curve")


def sparkle(img, at, size):
    """Soft glow with a tapered four-point glint."""
    g = layer()
    d = ImageDraw.Draw(g)
    x, y = P(*at)
    s = size * SS * K
    d.ellipse((x - s * 0.6, y - s * 0.6, x + s * 0.6, y + s * 0.6), fill=(255, 210, 255, 140))
    img.alpha_composite(g.filter(ImageFilter.GaussianBlur(s * 0.5)))
    star = []
    for i in range(8):
        a = math.pi / 4 * i
        r = s * (1.5 if i % 2 == 0 else 0.18)
        star.append((x + r * math.cos(a), y + r * math.sin(a)))
    glint = layer()
    ImageDraw.Draw(glint).polygon(star, fill=(255, 248, 255, 235))
    img.alpha_composite(glint.filter(ImageFilter.GaussianBlur(SS * 0.6)))


def star_points(cx, cy, R, r):
    pts = []
    for i in range(10):
        a = math.radians(-90 + 36 * i)
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    return pts


def star(img, cx, cy, R):
    """The five-pointed purple star with a gold rim and bevelled facets."""
    outer = star_points(cx, cy, R, R * 0.382)
    d = ImageDraw.Draw(img)
    d.polygon([P(*p) for p in outer], fill=(74, 22, 112, 255))
    inner = star_points(cx, cy, R - 34, (R - 34) * 0.382)
    for i in range(0, 10, 2):
        tip, prev, nxt = inner[i], inner[i - 1], inner[(i + 1) % 10]
        d.polygon([P(cx, cy), P(*tip), P(*prev)], fill=(150, 92, 206, 255))   # lit facet
        d.polygon([P(cx, cy), P(*tip), P(*nxt)], fill=(96, 40, 150, 255))     # shaded facet
    ridges = layer()
    rd = ImageDraw.Draw(ridges)
    for i in range(10):
        rd.line([P(cx, cy), P(*inner[i])], fill=(196, 150, 236, 110), width=round(2.5 * SS * K))
    img.alpha_composite(ridges.filter(ImageFilter.GaussianBlur(SS)))
    gold_outline(img, inner, 0.45)
    gold_outline(img, outer, 1.6)


def draw(path):
    img = layer()
    magenta = ((150, 30, 170, 255), (196, 70, 222, 255), (236, 150, 250, 255))
    purple = ((62, 10, 86, 255), (98, 28, 132, 255), (150, 80, 186, 255))

    star(img, 525, 600, 500)

    # Petals, back to front.
    tl = leaf((480, 640), (270, 200), 140, bulge=0.42)
    right = leaf((540, 650), (900, 540), 90, bulge=0.30, sharp=1.2)
    left = leaf((490, 650), (230, 900), 240, bulge=0.55)
    low = leaf((500, 760), (420, 990), 70, bulge=0.4)
    for pts, cols, hi in ((tl, magenta, leaf((470, 610), (305, 270), 38)),
                          (right, purple, leaf((580, 630), (850, 555), 24)),
                          (left, magenta, leaf((440, 690), (280, 840), 56)),
                          (low, magenta, None)):
        enamel(img, pts, *cols, hi_pts=hi)
        gold_outline(img, pts)

    # The lower body: dark purple, bird-like - a small rounded "head" on
    # the left, the body sweeping down to a pointed tail on the right.
    lip = catmull([(405, 790), (430, 745), (475, 740), (505, 770), (535, 720), (600, 700),
                   (665, 760), (705, 850), (735, 960), (650, 930), (560, 960), (475, 945),
                   (430, 895), (405, 840)])
    enamel(img, lip, *purple, hi_pts=catmull([(430, 790), (470, 765), (500, 800), (480, 880), (440, 860)]))
    gold_outline(img, lip)
    # the line where the "head" meets the body
    ImageDraw.Draw(img).line([P(505, 772), P(495, 830), P(470, 900)], fill=GOLD, width=round(6 * SS * K), joint="curve")

    # The nightingale: green-yellow, perched on the flower, beak to the right.
    bird = catmull([(440, 705), (448, 620), (482, 555), (520, 500), (552, 462), (592, 455),
                    (622, 476), (690, 498), (626, 516), (606, 548), (622, 610), (600, 668),
                    (555, 705), (495, 718)], n=20)
    enamel(img, bird, (110, 176, 40, 255), (168, 214, 64, 255), (232, 248, 140, 255),
           hi_pts=catmull([(470, 650), (490, 570), (540, 500), (585, 480), (560, 540), (510, 640)]))
    gold_outline(img, bird)
    eye = P(585, 488)
    r = 6 * SS * K
    ImageDraw.Draw(img).ellipse((eye[0] - r, eye[1] - r, eye[0] + r, eye[1] + r), fill=(40, 60, 20, 255))

    sparkle(img, (455, 735), 22)
    sparkle(img, (300, 300), 12)

    d = ImageDraw.Draw(img)
    cx = W * SS // 2
    d.text((cx, (H - 78) * SS), "Xarıbülbül", font=ImageFont.truetype(FONT, 38 * SS), fill=(226, 200, 236, 255), anchor="mm")
    d.text((cx, (H - 38) * SS), "Şuşa · Qarabağ", font=ImageFont.truetype(FONT_SANS, 15 * SS), fill=(150, 160, 176, 255), anchor="mm")

    img.resize((W, H), Image.LANCZOS).save(path, optimize=True)


if __name__ == "__main__":
    draw(sys.argv[1] if len(sys.argv) > 1 else "xari-bulbul.png")
