#!/usr/bin/env python3
"""sprites_all.py EXTRACTED_FILES_DIR OUT_DIR -- decode every BATTLE/*.SPR (palette 0) into OUT_DIR/<name>.png and build
a contact sheet OUT_DIR/_contact.png plus a size/height report. Same layout assumptions as spr2png.py."""
import struct, sys, os, glob
from PIL import Image, ImageDraw

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)

def decode(path, pal_index=0):
    data = open(path, 'rb').read()
    pal = []
    for i in range(16):
        c = struct.unpack_from('<H', data, pal_index * 32 + i * 2)[0]
        r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
        pal.append((r | r >> 5, g | g >> 5, b | b >> 5, 0 if i == 0 else 255))
    body = data[0x200:]
    h = len(body) // 128
    img = Image.new('RGBA', (256, h))
    px = img.load()
    for y in range(h):
        row = body[y * 128:(y + 1) * 128]
        for x in range(128):
            b = row[x]
            px[x * 2, y] = pal[b & 15]
            px[x * 2 + 1, y] = pal[b >> 4]
    return img, len(data)

files = sorted(glob.glob(os.path.join(src, 'BATTLE', '*.SPR')))
thumbs, sizes = [], {}
for f in files:
    img, n = decode(f)
    name = os.path.splitext(os.path.basename(f))[0]
    sizes[name] = (n, img.height)
    bg = Image.new('RGBA', img.size, (70, 70, 90, 255)); bg.alpha_composite(img)
    bg.convert('RGB').save(os.path.join(out, name + '.png'))
    # thumbnail: crop to the region that actually has pixels (top part), scale 1x
    bbox = img.getbbox()
    crop = bg.crop((0, 0, 256, min(img.height, (bbox[3] + 8) if bbox else 64)))
    thumbs.append((name, crop.convert('RGB')))

cols, cw, ch = 10, 256, 176
rows = (len(thumbs) + cols - 1) // cols
sheet = Image.new('RGB', (cols * cw, rows * ch), (40, 40, 55))
d = ImageDraw.Draw(sheet)
for i, (name, im) in enumerate(thumbs):
    x, y = (i % cols) * cw, (i // cols) * ch
    im2 = im.crop((0, 0, 256, ch - 14))
    sheet.paste(im2, (x, y + 14))
    d.text((x + 3, y + 2), name, fill=(255, 255, 200))
sheet.save(os.path.join(out, '_contact.png'))
print(f'{len(files)} sheets decoded -> {out}; contact sheet {sheet.size}')
from collections import Counter
print('file size -> count:', Counter(n for n, _ in sizes.values()).most_common(8))
print('image heights:', Counter(h for _, h in sizes.values()).most_common(8))
