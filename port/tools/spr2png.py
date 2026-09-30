#!/usr/bin/env python3
"""spr2png.py -- render an FFT unit sprite sheet (.SPR from the disc's BATTLE folder) to PNG (needs Pillow).

Layout used here (from the file sizes and inspection; verify visually):
  0x000-0x1FF  16 palettes x 16 colours, PS1 15-bit BGR (bit15 = semi-transparency flag), colour 0 transparent
  0x200-       4-bit pixels, 256 px wide (128 bytes/row), low nibble = left pixel
Usage: spr2png.py FILE.SPR OUT.png [--palette N] [--scale K]
"""
import argparse
import struct
from PIL import Image

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst')
    ap.add_argument('--palette', type=int, default=0)
    ap.add_argument('--scale', type=int, default=1)
    a = ap.parse_args()
    data = open(a.src, 'rb').read()
    pal = []
    for i in range(16):
        off = a.palette * 32 + i * 2
        c = struct.unpack_from('<H', data, off)[0]
        r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
        pal.append((r | r >> 5, g | g >> 5, b | b >> 5, 0 if i == 0 else 255))
    body = data[0x200:]
    w = 256
    h = len(body) // (w // 2)
    img = Image.new('RGBA', (w, h))
    px = img.load()
    for y in range(h):
        row = body[y * 128:(y + 1) * 128]
        for x in range(128):
            b = row[x]
            px[x * 2, y] = pal[b & 15]
            px[x * 2 + 1, y] = pal[b >> 4]
    if a.scale > 1:
        img = img.resize((w * a.scale, h * a.scale), Image.NEAREST)
    # composite on a mid-grey so transparent areas are visible
    bg = Image.new('RGBA', img.size, (70, 70, 90, 255))
    bg.alpha_composite(img)
    bg.convert('RGB').save(a.dst)
    print(f'{a.src}: {len(data)} bytes -> {w}x{h} (palette {a.palette}) -> {a.dst}')

if __name__ == '__main__':
    main()
