#!/usr/bin/env python3
"""shp_frames.py EXTRACTED_FILES_DIR SPR SHP OUT.png [--set primary|secondary] [--first N] [--count N] [--scale K]

Assemble the frames described by an FFT sprite frame table (.SHP) from a sprite sheet (.SPR).

Format (read from battle_gfx_unpack_unit_shp_data / battle_gfx_load_unit_frame_parts in the decomp):
  SHP:   u32 header (offset of the second table set, or 8 if only one)  u16 attack_frame_start  u16 sp2_frame_start
         0x100 x u32 frame offsets (only the first 0xd0 are used; -1 = none) relative to the frame blob
         u16 blob_size, then the blob
  frame: u8 (part_count-1) | (rotation_index << 3),  u8 flags,  then 4-byte parts
  part:  s8 x_shift, s8 y_shift, u16 attributes: bits 0-9 tile (u = (tile&0x1f)*8, v = (tile>>5)*8 in the sheet),
         bits 10-13 size index into g_battle_gfx_part_sizes (0x800946c8 in BATTLE.BIN: {s32 w,h} in 8-px units),
         bits 14-15 flip flags
Part origin is assumed to be the unit's ground point; the canvas anchors it at (cx, cy) -- check visually."""
import argparse, struct, os
from PIL import Image

PART_SIZES_ADDR, BATTLE_LOAD = 0x800946c8, 0x80067000

def load_sheet(path):
    data = open(path, 'rb').read()
    pal = []
    for i in range(16):
        c = struct.unpack_from('<H', data, i * 2)[0]
        r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
        pal.append((r | r >> 5, g | g >> 5, b | b >> 5, 0 if i == 0 else 255))
    body = data[0x200:]
    h = len(body) // 128
    im = Image.new('RGBA', (256, h))
    px = im.load()
    for y in range(h):
        row = body[y * 128:(y + 1) * 128]
        for x in range(128):
            px[x * 2, y] = pal[row[x] & 15]
            px[x * 2 + 1, y] = pal[row[x] >> 4]
    return im

def part_sizes(root):
    b = open(os.path.join(root, 'BATTLE.BIN'), 'rb').read()
    off = PART_SIZES_ADDR - BATTLE_LOAD
    return [struct.unpack_from('<ii', b, off + i * 8) for i in range(16)]

def frame_tables(shp, which):
    """Table of 0x100 u32 frame offsets at `base`, u16 blob size at base+0x400, blob at base+0x402.
    base = 8 for the primary set; the secondary set is at `header` unless header == 8 (then it repeats the primary)."""
    header = struct.unpack_from('<I', shp, 0)[0]
    base = header if (which == 'secondary' and header != 8) else 8
    offs = struct.unpack_from('<256I', shp, base)
    count = struct.unpack_from('<H', shp, base + 0x400)[0]
    return offs, shp[base + 0x402:base + 0x402 + count]

def assemble(sheet, blob, off, sizes, canvas=(96, 96), anchor=(48, 72)):
    im = Image.new('RGBA', canvas, (0, 0, 0, 0))
    hdr, flags = blob[off], blob[off + 1]
    n = (hdr & 7) + 1
    for k in range(n):
        xs, ys, attr = struct.unpack_from('<bbH', blob, off + 2 + k * 4)
        tile, size, flip = attr & 0x3ff, (attr >> 10) & 15, (attr >> 14) & 3
        w, h = sizes[size][0] * 8, sizes[size][1] * 8
        if w <= 0 or h <= 0:
            continue
        u, v = (tile & 0x1f) * 8, (tile >> 5) * 8
        if v + h > sheet.height or u + w > 256:
            continue
        part = sheet.crop((u, v, u + w, v + h))
        if flip & 1:
            part = part.transpose(Image.FLIP_LEFT_RIGHT)
        if flip & 2:
            part = part.transpose(Image.FLIP_TOP_BOTTOM)
        px, py = anchor[0] + xs, anchor[1] + ys
        if 0 <= px and 0 <= py and px + w <= canvas[0] and py + h <= canvas[1]:
            im.alpha_composite(part, (px, py))
    return im, n, flags

def export_json(root, shp_name, dst):
    """Dump every frame's parts (source rect in the sheet, offset, flips) as JSON -- the data a sprite-replacement tool needs."""
    import json
    shp = open(os.path.join(root, shp_name), 'rb').read()
    sizes = part_sizes(root)
    header = struct.unpack_from('<I', shp, 0)[0]
    out = {'file': shp_name, 'second_set': header != 8,
           'attack_frame_start': struct.unpack_from('<H', shp, 4)[0], 'sp2_frame_start': struct.unpack_from('<H', shp, 6)[0],
           'sets': {}}
    for which in (['primary', 'secondary'] if header != 8 else ['primary']):
        offs, blob = frame_tables(shp, which)
        frames = {}
        for i in range(0xd0):
            o = offs[i]
            if o == 0xffffffff or o + 2 > len(blob):
                continue
            n = (blob[o] & 7) + 1
            parts = []
            for k in range(n):
                if o + 2 + k * 4 + 4 > len(blob):
                    break
                xs, ys, attr = struct.unpack_from('<bbH', blob, o + 2 + k * 4)
                tile, size = attr & 0x3ff, (attr >> 10) & 15
                parts.append({'u': (tile & 0x1f) * 8, 'v': (tile >> 5) * 8, 'w': sizes[size][0] * 8, 'h': sizes[size][1] * 8,
                              'x': xs, 'y': ys, 'flip': (attr >> 14) & 3})
            frames[i] = {'flags': blob[o + 1], 'rotation_index': blob[o] >> 3, 'parts': parts}
        out['sets'][which] = frames
    json.dump(out, open(dst, 'w'), indent=1)
    return sum(len(f) for f in out['sets'].values())

def main():
    ap = argparse.ArgumentParser()
    if len(__import__('sys').argv) > 1 and __import__('sys').argv[1] == '--json':
        _, _, root, shp_name, dst = __import__('sys').argv
        print(f'{shp_name}: {export_json(root, shp_name, dst)} frames -> {dst}')
        return
    ap.add_argument('root'); ap.add_argument('spr'); ap.add_argument('shp'); ap.add_argument('dst')
    ap.add_argument('--set', default='primary')
    ap.add_argument('--first', type=int, default=0); ap.add_argument('--count', type=int, default=48)
    ap.add_argument('--scale', type=int, default=2)
    a = ap.parse_args()
    sheet = load_sheet(os.path.join(a.root, a.spr))
    shp = open(os.path.join(a.root, a.shp), 'rb').read()
    sizes = part_sizes(a.root)
    offs, blob = frame_tables(shp, a.set)
    cw, ch, cols = 96, 96, 10
    sel = list(range(a.first, min(a.first + a.count, 0xd0)))
    rows = (len(sel) + cols - 1) // cols
    atlas = Image.new('RGBA', (cols * cw, rows * ch), (60, 60, 80, 255))
    used = 0
    for i, fi in enumerate(sel):
        o = offs[fi]
        if o == 0xffffffff or o + 2 > len(blob):
            continue
        fr, n, flags = assemble(sheet, blob, o, sizes)
        atlas.alpha_composite(fr, ((i % cols) * cw, (i // cols) * ch))
        used += 1
    if a.scale > 1:
        atlas = atlas.resize((atlas.width * a.scale, atlas.height * a.scale), Image.NEAREST)
    atlas.convert('RGB').save(a.dst)
    print(f'part sizes (w,h in 8px tiles): {sizes}')
    print(f'{a.shp} {a.set}: {used} frames assembled from {a.spr} -> {a.dst}')

if __name__ == '__main__':
    main()
