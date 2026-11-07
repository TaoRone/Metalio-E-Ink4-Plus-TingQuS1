# -*- coding: utf-8 -*-
"""Generate cover/featured images for the TingQu S1 project.

Outputs:
  images/tingqus1-cover.png        1600x900  (16:9)
  images/tingqus1-cover-square.png 1200x1200 (1:1)
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRODUCT = os.path.join(ROOT, "images", "product.png")

INK = (22, 24, 26)          # #16181a
DIM = (122, 125, 128)       # #7a7d80
ACCENT = (20, 83, 45)       # #14532d
LINE = (230, 228, 218)
WHITE = (255, 255, 255)
OUTLINE_GRAY = (170, 174, 170)

FONT_DIR = "C:/Windows/Fonts"
def font(size, bold=False):
    name = "msyhbd.ttc" if bold else "msyh.ttc"
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def make_canvas(w, h):
    bg = Image.new("RGB", (w, h))
    px = bg.load()
    c0, c1 = (247, 246, 239), (232, 239, 233)
    for yy in range(h):
        for xx in range(0, w, 4):
            t = (xx / w * 0.55 + yy / h * 0.45)
            for dx in range(4):
                if xx + dx < w:
                    px[xx + dx, yy] = lerp(c0, c1, t)
    img = bg.convert("RGBA")
    deco = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dd = ImageDraw.Draw(deco)
    dd.ellipse([w - 380, -200, w + 200, 380], fill=(20, 83, 45, 14))
    dd.ellipse([-180, h - 320, 280, h + 160], fill=(20, 83, 45, 10))
    img.alpha_composite(deco)
    return img


def rounded_card_with_shadow(base, img, box, radius, shadow_blur, shadow_alpha):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    card = img.resize((w, h), Image.LANCZOS).convert("RGB")

    sh = Image.new("RGBA", base.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sh)
    sd.rounded_rectangle([x0, y0 + 14, x1, y1 + 22], radius, fill=(20, 40, 28, shadow_alpha))
    sh = sh.filter(ImageFilter.GaussianBlur(shadow_blur))
    base.alpha_composite(sh)

    mask = Image.new("L", (w, h), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([0, 0, w, h], radius, fill=255)
    base.paste(card, (x0, y0), mask)

    bd = ImageDraw.Draw(base)
    bd.rounded_rectangle([x0, y0, x1, y1], radius, outline=(205, 203, 195), width=2)


def _pill_rect(d, text, f, pad_x, pad_y):
    bbox = d.textbbox((0, 0), text, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    return tw + pad_x * 2, th + pad_y * 2, bbox


def draw_pill(d, x, y, text, f, fg, bg=None, outline=None, pad_x=26, pad_y=13):
    w, h, bbox = _pill_rect(d, text, f, pad_x, pad_y)
    r = h // 2
    rect = [x, y, x + w, y + h]
    if bg is not None:
        d.rounded_rectangle(rect, r, fill=bg)
    if outline is not None:
        d.rounded_rectangle(rect, r, outline=outline, width=2)
    d.text((x + pad_x - bbox[0], y + pad_y - bbox[1]), text, font=f, fill=fg)
    return w


PILL_STYLES = {
    "ink":     dict(fg=WHITE, bg=INK),
    "outline": dict(fg=INK, outline=OUTLINE_GRAY),
    "green":   dict(fg=WHITE, bg=ACCENT),
}


def draw_pill_row(d, cx, y, specs, f, gap=16, pad_x=26, pad_y=14):
    widths = [_pill_rect(d, t, f, pad_x, pad_y)[0] for t, _ in specs]
    total = sum(widths) + gap * (len(specs) - 1)
    x = int(cx - total / 2)
    for (text, style), w in zip(specs, widths):
        draw_pill(d, x, y, text, f, **PILL_STYLES[style], pad_x=pad_x, pad_y=pad_y)
        x += w + gap


def draw_dashed_badge(d, cx, y, text, f, pad_x=24, pad_y=11):
    w, h, bbox = _pill_rect(d, text, f, pad_x, pad_y)
    x = int(cx - w / 2)
    r = h // 2
    rect = [x, y, x + w, y + h]
    # dashed rounded outline
    dash, gap = 10, 7
    def dashed_line(p0, p1):
        (ax, ay), (bx, by) = p0, p1
        length = ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
        if length <= 2:
            return
        n = max(1, int(length // (dash + gap)))
        for i in range(n + 1):
            t0 = i * (dash + gap) / length
            t1 = min(1, t0 + dash / length)
            if t0 >= 1:
                break
            d.line([(ax + (bx - ax) * t0, ay + (by - ay) * t0),
                    (ax + (bx - ax) * t1, ay + (by - ay) * t1)], fill=INK, width=2)
    x0, y0, x1, y1 = rect
    dashed_line((x0 + r, y0), (x1 - r, y0))
    dashed_line((x0 + r, y1), (x1 - r, y1))
    if y1 - 2 * r > 2:
        dashed_line((x0, y0 + r), (x0, y1 - r))
        dashed_line((x1, y0 + r), (x1, y1 - r))
    for ccx, ccy, start in [(x0 + r, y0 + r, 180), (x1 - r, y0 + r, 270),
                            (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90)]:
        d.arc([ccx - r, ccy - r, ccx + r, ccy + r], start, start + 90, fill=INK, width=2)
    d.text((x + pad_x - bbox[0], y + pad_y - bbox[1]), text, font=f, fill=INK)


def center_text(d, cx, y, text, f, fill):
    bbox = d.textbbox((0, 0), text, font=f)
    d.text((int(cx - (bbox[2] - bbox[0]) / 2) - bbox[0], y), text, font=f, fill=fill)


def render_wide():
    W, H = 1600, 900
    img = make_canvas(W, H)
    d = ImageDraw.Draw(img)
    prod = Image.open(PRODUCT)

    LX = 100
    cx = W / 2

    draw_dashed_badge(d, LX + 258, 148, "Metalio E-Ink4 Plus · 第三方开源固件", font(20))

    f_title = font(110, bold=True)
    d.text((LX - 4, 218), "听趣", font=f_title, fill=INK)
    tw = d.textbbox((0, 0), "听趣", font=f_title)[2]
    d.text((LX - 4 + tw + 18, 218), "S1", font=f_title, fill=ACCENT)

    d.text((LX, 380), "你的墨水屏阅读伴侣", font=font(40, bold=True), fill=INK)
    d.text((LX, 452), "微信读书进度随身带 · 网盘书库直接读 · 4G 永远在线",
           font=font(23), fill=DIM)

    draw_pill_row(d, LX + 330, 536, [("微信读书同步", "ink"), ("4G + WiFi 双在线", "outline"),
                                     ("官方级省电", "outline"), ("100% 开源 MIT", "green")], font(21, bold=True))

    d.line([(LX, 660), (LX + 620, 660)], fill=LINE, width=2)
    d.text((LX, 686), "4.7 寸冷暖前光墨水屏 · 书架 / 进度 / 笔记实时同步 · 一次充电读几周",
           font=font(19), fill=DIM)

    for i in range(5):
        s = 10
        d.rectangle([LX + i * 26, 762, LX + i * 26 + s, 762 + s],
                    fill=ACCENT if i == 0 else (180, 200, 188))

    rounded_card_with_shadow(img, prod, (950, 140, 1510, 760), radius=28,
                             shadow_blur=26, shadow_alpha=60)

    out = os.path.join(ROOT, "images", "tingqus1-cover.png")
    img.convert("RGB").save(out, "PNG", optimize=True)
    print("saved:", out, os.path.getsize(out), "bytes")


def render_square():
    S = 1200
    img = make_canvas(S, S)
    d = ImageDraw.Draw(img)
    prod = Image.open(PRODUCT)
    cx = S / 2

    draw_dashed_badge(d, cx, 78, "Metalio E-Ink4 Plus · 第三方开源固件", font(19))

    # title: 听趣 (ink) + S1 (green), centered as a whole
    f_title = font(102, bold=True)
    t1, t2 = "听趣", "S1"
    w1 = d.textbbox((0, 0), t1, font=f_title)[2]
    w2 = d.textbbox((0, 0), t2, font=f_title)[2]
    gap = 20
    x = int(cx - (w1 + gap + w2) / 2)
    d.text((x, 136), t1, font=f_title, fill=INK)
    d.text((x + w1 + gap, 136), t2, font=f_title, fill=ACCENT)

    center_text(d, cx, 268, "你的墨水屏阅读伴侣", font(38, bold=True), INK)
    center_text(d, cx, 334, "微信读书进度随身带 · 网盘书库直接读 · 4G 永远在线", font(22), DIM)

    # product card
    card = 560
    rounded_card_with_shadow(img, prod, (int(cx - card / 2), 396, int(cx + card / 2), 396 + card),
                             radius=28, shadow_blur=26, shadow_alpha=60)
    d = ImageDraw.Draw(img)

    f_pill = font(22, bold=True)
    draw_pill_row(d, cx, 1002, [("微信读书同步", "ink"), ("4G + WiFi 双在线", "outline")], f_pill, gap=20)
    draw_pill_row(d, cx, 1078, [("官方级省电", "outline"), ("100% 开源 MIT", "green")], f_pill, gap=20)

    out = os.path.join(ROOT, "images", "tingqus1-cover-square.png")
    img.convert("RGB").save(out, "PNG", optimize=True)
    print("saved:", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    render_wide()
    render_square()
