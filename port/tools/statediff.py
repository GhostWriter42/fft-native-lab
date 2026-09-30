#!/usr/bin/env python3
"""statediff.py -- compare two machine-state snapshots written by the lockstep driver (-SnapSave) and list where they differ.

  python port/tools/statediff.py A.state B.state [--symbols port/build/native/ls/symbols_pc.ld] [--max 60]

A snapshot (lockstep.c, "snapshots") is: u32 magic 'FFTS', version, nregions, frame, image end, address of main_test, then (address, size) per region, then for every 4 KiB page of every
region one flag byte (0 = all zero, 1 = data follows, 2 = excluded) and the data. Regions: the program's writable image (.data/.bss), PS1 RAM, scratchpad, the thread-stack window,
the coverage thunks. Differences are reported per region as runs of words; RAM addresses (0x80......) are resolved to the nearest symbol of the linker script when --symbols is given.
"""
import argparse
import bisect
import re
import struct
import sys
from pathlib import Path

FIXED_NAMES = ['PS1 RAM', 'scratchpad', 'thread-stack window', 'coverage thunks']


def load(path):
    b = Path(path).read_bytes()
    magic, ver, n, frame, end, main_test = struct.unpack_from('<6I', b, 0)
    if magic != 0x53544646:
        sys.exit(f'{path}: not a state file')
    pos = 24
    regs = []
    for _ in range(n):
        regs.append(struct.unpack_from('<2I', b, pos))
        pos += 8
    mem = {}
    for lo, size in regs:
        hi = lo + size
        data = bytearray(size)
        pg = lo & ~0xfff
        while pg < hi:
            a = max(pg, lo)
            z = min(pg + 4096, hi)
            flag = b[pos]
            pos += 1
            if flag == 1:
                data[a - lo:z - lo] = b[pos:pos + (z - a)]
                pos += z - a
            elif flag == 2:
                data[a - lo:z - lo] = b'\xee' * (z - a)
            pg += 4096
        mem[lo] = bytes(data)
    return frame, regs, mem


def symbols(path):
    tab = []
    for line in Path(path).read_text().splitlines():
        m = re.match(r'(\S+)\s*=\s*(0x[0-9a-fA-F]+);', line)
        if m:
            tab.append((int(m.group(2), 16), m.group(1)))
    tab.sort()
    return [a for a, _ in tab], [n for _, n in tab]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--symbols')
    ap.add_argument('--max', type=int, default=40, help='differing runs to list per region')
    args = ap.parse_args()
    fa, ra, ma = load(args.a)
    fb, rb, mb = load(args.b)
    if ra != rb:
        sys.exit('the two states have different region layouts (different program builds)')
    print(f'frames: {fa} vs {fb}')
    saddr, sname = symbols(args.symbols) if args.symbols else ([], [])
    for idx, (lo, size) in enumerate(ra):
        da, db = ma[lo], mb[lo]
        nimg = len(ra) - len(FIXED_NAMES)
        name = 'program image (.data/.bss)' if idx < nimg else FIXED_NAMES[idx - nimg]
        if da == db:
            print(f'{name} @0x{lo:08x} +0x{size:x}: identical')
            continue
        words = [(i, struct.unpack_from('<I', da, i)[0], struct.unpack_from('<I', db, i)[0]) for i in range(0, size - 3, 4) if da[i:i + 4] != db[i:i + 4]]
        print(f'{name} @0x{lo:08x} +0x{size:x}: {len(words)} words differ')
        runs = []
        for i, x, y in words:
            if runs and i - runs[-1][1] <= 4:
                runs[-1][1] = i
                runs[-1][2] += 1
            else:
                runs.append([i, i, 1, x, y])
        for i0, i1, n, x, y in runs[:args.max]:
            addr = lo + i0
            sym = ''
            if saddr and 0x80000000 <= addr < 0x80200000:
                k = bisect.bisect_right(saddr, addr) - 1
                if k >= 0:
                    sym = f'  ({sname[k]}+0x{addr - saddr[k]:x})'
            print(f'  0x{addr:08x}..0x{lo + i1 + 3:08x} ({n} words): 0x{x:08x} vs 0x{y:08x}{sym}')
        if len(runs) > args.max:
            print(f'  ... {len(runs) - args.max} more runs')


if __name__ == '__main__':
    main()
