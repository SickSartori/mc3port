# Makes res/mc3.ico and docs/logo.png:
#   python tools/make_icon.py third_party/fonts/BarlowSemiCondensed-BlackItalic.ttf <output folder>
"""The mc3port icon: "MC3" in Barlow Semi Condensed Black Italic, styled like the game's logo (red gradient
letters, white chrome outline, dark outer edge) on a dark tile with a red glow and a leaning red bar."""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

FONT = sys.argv[1]
OUT = sys.argv[2]          # folder for icon-256.png (docs/logo.png), icon-sheet.png and mc3.ico (res/mc3.ico)
S = 1024                   # master size; every icon size is drawn from it


def vertical_gradient(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", size)
    px = g.load()
    for y in range(h):
        t = y / (h - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(4))
        for x in range(w):
            px[x, y] = c
    return g


def tile(size):
    """Rounded dark square, lighter at the top, a red glow low right (like the launcher's background)."""
    img = vertical_gradient((size, size), (34, 33, 36, 255), (10, 10, 12, 255))
    glow = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(glow)
    d.ellipse((size * 0.35, size * 0.55, size * 1.25, size * 1.35), fill=150)
    glow = glow.filter(ImageFilter.GaussianBlur(size * 0.12))
    red = Image.new("RGBA", (size, size), (190, 18, 28, 255))
    img = Image.composite(red, img, glow)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=size * 0.2, fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    # A thin light rim, like the launcher's panels.
    rim = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle((size * 0.006, size * 0.006, size * 0.994, size * 0.994), radius=size * 0.2,
                                          outline=(255, 255, 255, 34), width=max(1, round(size * 0.008)))
    return Image.alpha_composite(out, rim)


def text_mask(size, text, height_ratio, centre_y):
    """The word as a mask, scaled to `height_ratio` of the tile and centred."""
    font = ImageFont.truetype(FONT, size)
    box = font.getbbox(text)
    tmp = Image.new("L", (box[2] - box[0] + size, box[3] - box[1] + size), 0)
    ImageDraw.Draw(tmp).text((size // 2 - box[0], size // 2 - box[1]), text, font=font, fill=255)
    tmp = tmp.crop(tmp.getbbox())
    scale = S * height_ratio / tmp.height
    if tmp.width * scale > S * 0.84:  # never wider than the tile's inside
        scale = S * 0.84 / tmp.width
    tmp = tmp.resize((round(tmp.width * scale), round(tmp.height * scale)), Image.LANCZOS)
    mask = Image.new("L", (S, S), 0)
    mask.paste(tmp, ((S - tmp.width) // 2, round(S * centre_y - tmp.height / 2)))
    return mask


def grow(mask, pixels):
    return mask.filter(ImageFilter.MaxFilter(pixels * 2 + 1)) if pixels > 0 else mask


def master(chrome, edge):
    img = tile(S)
    # The leaning bar under the word: red to dark red, with the chrome sliver in front (the launcher's menu bar).
    bar = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(bar)
    y0, y1, skew = S * 0.70, S * 0.78, S * 0.016
    d.polygon([(S * 0.20 + skew, y0), (S * 0.86 + skew, y0), (S * 0.86, y1), (S * 0.20, y1)], fill=(200, 24, 34, 255))
    d.polygon([(S * 0.14 + skew, y0), (S * 0.17 + skew, y0), (S * 0.17, y1), (S * 0.14, y1)], fill=(217, 218, 223, 255))
    img = Image.alpha_composite(img, bar)

    word = text_mask(560, "MC3", 0.40, 0.45)
    outer = grow(word, chrome + edge)
    white = grow(word, chrome)
    # Dark outer edge, then the chrome (white to grey), then the red letters (bright to dark red).
    shadow = outer.filter(ImageFilter.GaussianBlur(S * 0.012))
    img = Image.composite(Image.new("RGBA", (S, S), (0, 0, 0, 230)), img, ImageChops.offset(shadow, round(S * 0.01), round(S * 0.016)))
    img = Image.composite(Image.new("RGBA", (S, S), (14, 6, 8, 255)), img, outer)
    img = Image.composite(vertical_gradient((S, S), (255, 255, 255, 255), (165, 168, 176, 255)), img, white)
    red = vertical_gradient((S, S), (255, 86, 92, 255), (150, 10, 18, 255))
    # A shine across the upper half of the letters, like the logo's lacquer.
    shine = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shine).rectangle((0, 0, S, S * 0.43), fill=60)
    red = Image.composite(Image.new("RGBA", (S, S), (255, 255, 255, 255)), red, shine)
    img = Image.composite(red, img, word)
    return img


big = master(chrome=14, edge=16)    # 48 px and up: the full outline
small = master(chrome=30, edge=26)  # 16-32 px: thicker outlines, or they vanish
sizes = [256, 128, 64, 48, 40, 32, 24, 20, 16]
frames = [(big if n >= 48 else small).resize((n, n), Image.LANCZOS) for n in sizes]
big.resize((256, 256), Image.LANCZOS).save(OUT + "/icon-256.png")
sheet = Image.new("RGBA", (256 + 16 + sum(n + 16 for n in sizes[1:]), 256), (40, 40, 44, 255))
x = 0
for f in frames:
    sheet.paste(f, (x, 256 - f.height), f)
    x += f.width + 16
sheet.save(OUT + "/icon-sheet.png")
frames[0].save(OUT + "/mc3.ico", format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[1:])
print("ok")
