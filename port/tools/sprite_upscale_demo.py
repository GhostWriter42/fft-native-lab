#!/usr/bin/env python3
"""sprite_upscale_demo.py SPR OUT.png [--frame X Y W H] -- upscaling in INDEX space keeps palette swaps working.

Decodes a unit sprite sheet to a 2-D array of palette indices (0-15), upscales the *indices* with Scale2x (EPX, applied
twice = 4x), then applies palettes afterwards. Because the upscaled image is still an index image, every one of the 16
palettes of the sheet (team/job colour swaps) recolours it correctly -- unlike an RGB upscale of one palette.
Output: rows = palettes 0..3 (left: nearest 4x; right: Scale2x x2 in index space)."""
import argparse, struct
from PIL import Image

def load(path):
    data = open(path, 'rb').read()
    pals = []
    for p in range(16):
        pal = []
        for i in range(16):
            c = struct.unpack_from('<H', data, p * 32 + i * 2)[0]
            r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
            pal.append((r | r >> 5, g | g >> 5, b | b >> 5, 0 if i == 0 else 255))
        pals.append(pal)
    body = data[0x200:]
    h = len(body) // 128
    idx = [[0] * 256 for _ in range(h)]
    for y in range(h):
        row = body[y * 128:(y + 1) * 128]
        for x in range(128):
            idx[y][x * 2] = row[x] & 15
            idx[y][x * 2 + 1] = row[x] >> 4
    return pals, idx

def scale2x(a):
    """Scale2x/EPX on a 2-D list of ints (works on palette indices)."""
    h, w = len(a), len(a[0])
    out = [[0] * (w * 2) for _ in range(h * 2)]
    for y in range(h):
        for x in range(w):
            p = a[y][x]
            b = a[y - 1][x] if y > 0 else p          # up
            d = a[y][x - 1] if x > 0 else p          # left
            f = a[y][x + 1] if x < w - 1 else p      # right
            hh = a[y + 1][x] if y < h - 1 else p     # down
            e0 = d if (d == b and b != hh and d != f) else p
            e1 = f if (b == f and b != hh and d != f) else p
            e2 = d if (d == hh and d != b and hh != f) else p
            e3 = f if (hh == f and d != b and hh != f) else p
            out[2 * y][2 * x], out[2 * y][2 * x + 1] = e0, e1
            out[2 * y + 1][2 * x], out[2 * y + 1][2 * x + 1] = e2, e3
    return out

def render(idx, pal, bg=(70, 70, 90, 255)):
    h, w = len(idx), len(idx[0])
    im = Image.new('RGBA', (w, h), bg)
    px = im.load()
    for y in range(h):
        for x in range(w):
            c = pal[idx[y][x]]
            if c[3]:
                px[x, y] = c
    return im

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst')
    ap.add_argument('--frame', type=int, nargs=4, default=[0, 0, 128, 40])
    a = ap.parse_args()
    pals, idx = load(a.src)
    x, y, w, h = a.frame
    crop = [row[x:x + w] for row in idx[y:y + h]]
    up = scale2x(scale2x(crop))
    near = [[v for v in row for _ in range(4)] for row in crop for _ in range(4)]
    tiles = []
    for p in range(4):
        tiles.append((render(near, pals[p]), render(up, pals[p])))
    W, H = tiles[0][0].size
    sheet = Image.new('RGB', (W * 2 + 8, H * 4 + 12), (30, 30, 40))
    for i, (l, r) in enumerate(tiles):
        sheet.paste(l.convert('RGB'), (0, i * (H + 4)))
        sheet.paste(r.convert('RGB'), (W + 8, i * (H + 4)))
    sheet.save(a.dst)
    print(f'{a.src}: crop {w}x{h} -> {W}x{H}; rows = palettes 0..3; left nearest 4x, right Scale2x x2 in index space -> {a.dst}')

if __name__ == '__main__':
    main()
