#!/usr/bin/env python3
"""compare_discs.py ORIGINAL.bin MODDED.bin [--file LBA SIZE LOAD_ADDR NAME]... [--functions battle.yaml]

Compare two raw Mode 2/2352 disc images sector by sector and map the differences back to files/functions.
Sector layout: 12 sync + 4 header + 8 subheader + 2048 user data + 4 EDC + 276 ECC (Mode 2 Form 1). A changed user byte also
changes that sector's EDC/ECC, so differing sectors are expected to differ in the tail too; only user-data bytes are reported."""
import argparse, re, sys
from pathlib import Path

SECTOR, USER_OFF, USER = 2352, 24, 2048

def sector(f, lba):
    f.seek(lba * SECTOR)
    return f.read(SECTOR)

def file_bytes(path, lba, size):
    out = bytearray()
    with open(path, 'rb') as f:
        n = (size + USER - 1) // USER
        for i in range(n):
            s = sector(f, lba + i)
            out += s[USER_OFF:USER_OFF + USER]
    return bytes(out[:size])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('orig'); ap.add_argument('mod')
    ap.add_argument('--file', nargs=4, action='append', metavar=('LBA', 'SIZE', 'LOAD', 'NAME'), default=[])
    ap.add_argument('--functions')
    a = ap.parse_args()
    so, sm = Path(a.orig).stat().st_size, Path(a.mod).stat().st_size
    print(f'sizes: original {so} bytes, modded {sm} bytes ({"same" if so == sm else "DIFFERENT"})')
    diff_sectors = []
    with open(a.orig, 'rb') as fo, open(a.mod, 'rb') as fm:
        i = 0
        while True:
            x, y = fo.read(SECTOR * 256), fm.read(SECTOR * 256)
            if not x and not y:
                break
            for k in range(0, max(len(x), len(y)), SECTOR):
                if x[k:k + SECTOR] != y[k:k + SECTOR]:
                    diff_sectors.append(i + k // SECTOR)
            i += 256
    print(f'differing sectors: {len(diff_sectors)}  LBAs: {diff_sectors[:12]}{" ..." if len(diff_sectors) > 12 else ""}')
    funcs = []
    if a.functions:
        for l in Path(a.functions).read_text().splitlines():
            m = re.match(r'\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*size:\s*(\d+),\s*name:\s*(\w+)', l)
            if m:
                funcs.append((int(m.group(1), 16), int(m.group(2)), m.group(3)))
    for lba, size, load, name in a.file:
        lba, size, load = int(lba), int(size), int(load, 16)
        bo, bm = file_bytes(a.orig, lba, size), file_bytes(a.mod, lba, size)
        idx = [i for i in range(size) if bo[i] != bm[i]]
        print(f'\n{name} (LBA {lba}, {size} bytes): {len(idx)} differing bytes')
        if idx:
            # group into runs
            runs, s, p = [], idx[0], idx[0]
            for i in idx[1:]:
                if i != p + 1:
                    runs.append((s, p)); s = i
                p = i
            runs.append((s, p))
            for s, e in runs:
                addr = load + s
                owner = next((n for (fa, fs, n) in funcs if fa <= addr < fa + fs), '?')
                print(f'  file +0x{s:x}..+0x{e:x} (addr 0x{addr:08x}..0x{load + e:08x}, {e - s + 1} bytes) in function {owner}'
                      f'  orig {bo[s:e+1].hex()}  mod {bm[s:e+1].hex()}')

if __name__ == '__main__':
    main()
