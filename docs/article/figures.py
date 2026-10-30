#!/usr/bin/env python3
"""Draw the article's figures (PNG, print resolution) with Pillow."""
from PIL import Image, ImageDraw, ImageFont
import os

OUT = os.path.dirname(os.path.abspath(__file__))
F = "/usr/share/fonts/truetype/liberation/LiberationSerif-%s.ttf"
reg = lambda s: ImageFont.truetype(F % "Regular", s)
bold = lambda s: ImageFont.truetype(F % "Bold", s)
ital = lambda s: ImageFont.truetype(F % "Italic", s)

def box(d, xy, text, font, fill="#ffffff", dashed=False, sub=None, subfont=None):
    x0, y0, x1, y1 = xy
    if dashed:
        d.rectangle(xy, fill=fill)
        for x in range(x0, x1, 24):
            d.line([(x, y0), (min(x + 12, x1), y0)], fill="#000", width=3)
            d.line([(x, y1), (min(x + 12, x1), y1)], fill="#000", width=3)
        for y in range(y0, y1, 24):
            d.line([(x0, y), (x0, min(y + 12, y1))], fill="#000", width=3)
            d.line([(x1, y), (x1, min(y + 12, y1))], fill="#000", width=3)
    else:
        d.rectangle(xy, fill=fill, outline="#000", width=3)
    cy = (y0 + y1) / 2 - (18 if sub else 0)
    d.text(((x0 + x1) / 2, cy), text, font=font, fill="#000", anchor="mm")
    if sub:
        d.text(((x0 + x1) / 2, cy + 40), sub, font=subfont, fill="#000", anchor="mm")

def arrow(d, a, b, w=3):
    d.line([a, b], fill="#000", width=w)
    import math
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    for s in (2.6, -2.6):
        d.line([b, (b[0] + 18 * math.cos(ang + s), b[1] + 18 * math.sin(ang + s))], fill="#000", width=w)

# ---- Figure 1: architecture ------------------------------------------------
W, H = 2000, 1080
im = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(im)
d.text((40, 30), "İstifadəçi sahəsi (Shusha)", font=bold(40), fill="#000")
box(d, (40, 100, 640, 230), "Explorer, Terminal", bold(38), sub="tətbiqlər", subfont=ital(32))
box(d, (700, 100, 1300, 230), "wm (kompozitor)", bold(38), sub="zədələnmiş sahələr", subfont=ital(32))
box(d, (1360, 100, 1960, 230), "Workspace", bold(38), "#f2f2f2", dashed=True, sub="birgə redaktə", subfont=ital(32))
box(d, (40, 290, 460, 420), "keyd", bold(38), "#f2f2f2", dashed=True, sub="açar anbarı", subfont=ital(32))
box(d, (540, 290, 960, 420), "authd", bold(38), "#f2f2f2", dashed=True, sub="hesablar, Argon2id", subfont=ital(32))
box(d, (1040, 290, 1460, 420), "netd", bold(38), "#f2f2f2", dashed=True, sub="Noise + ML-KEM", subfont=ital(32))
box(d, (1540, 290, 1960, 420), "workspaced", bold(38), "#f2f2f2", dashed=True, sub="jurnal, CRDT", subfont=ital(32))
d.rectangle((40, 480, 1960, 560), fill="#e6e6e6", outline="#000", width=3)
d.text((1000, 520), "IPC: mesaj növbələri (SYS_MSG_*) və paylaşılan yaddaş (SYS_SHM_*)", font=reg(36), fill="#000", anchor="mm")
d.text((40, 600), "ICDA nüvəsi", font=bold(40), fill="#000")
box(d, (40, 660, 500, 800), "CSPRNG", bold(38), sub="RDSEED/RDRAND + jitter", subfont=ital(30))
box(d, (540, 660, 1000, 800), "crypto/", bold(38), sub="AEAD, SHA-2, HKDF, Ed25519", subfont=ital(30))
box(d, (1040, 660, 1500, 800), "VFS, /dev/random", bold(38), sub="fayl sistemi", subfont=ital(30))
box(d, (1540, 660, 1960, 800), "planlayıcı", bold(38), sub="proseslər, IPC", subfont=ital(30))
box(d, (40, 850, 1960, 960), "Drayverlər: e1000, virtio-net, AHCI/ATA/NVMe, framebuffer, virtio-gpu, PS/2", reg(36))
d.rectangle((40, 1000, 90, 1040), outline="#000", width=3)
d.text((105, 1020), "reallaşdırılıb", font=reg(32), fill="#000", anchor="lm")
box(d, (420, 1000, 470, 1040), "", reg(10), "#f2f2f2", dashed=True)
d.text((485, 1020), "layihələndirilib, növbəti mərhələ", font=reg(32), fill="#000", anchor="lm")
im.save(os.path.join(OUT, "fig1_architecture.png"), dpi=(300, 300))

# ---- Figure 2: key hierarchy -------------------------------------------------
W, H = 2000, 820
im = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(im)
ranks = [("RK₀", "admin (rütbə 0)"), ("RK₁", "rütbə 1"), ("RK₂", "rütbə 2")]
for i, (k, lab) in enumerate(ranks):
    x = 60 + i * 420
    box(d, (x, 60, x + 340, 190), k, bold(46), sub=lab, subfont=ital(32))
    if i:
        arrow(d, (x - 80, 125), (x - 4, 125))
d.text((1350, 90), "Hər rütbə açarı özündən", font=reg(34), fill="#000")
d.text((1350, 135), "aşağı rütbənin açarını açır", font=reg(34), fill="#000")
box(d, (480, 330, 1180, 470), "FK (fayl açarı, təsadüfi 256 bit)", bold(40), sub="sahibi: rütbə 2", subfont=ital(32))
arrow(d, (650, 190), (760, 326))
d.text((610, 258), "wrap_sym(RK₁, FK)", font=ital(32), fill="#000", anchor="rm")
box(d, (1300, 330, 1940, 470), "sahibin açarı", bold(40), sub="wrap(pk_sahib, FK)", subfont=ital(32))
arrow(d, (1300, 400), (1184, 400))
box(d, (60, 610, 1940, 740), "Məzmun: 64 KiB hissələr, XChaCha20-Poly1305(FK, nonce, hissə, AAD) + Ed25519 imzası", reg(38))
arrow(d, (830, 470), (830, 606))
im.save(os.path.join(OUT, "fig2_keys.png"), dpi=(300, 300))
print("ok")
