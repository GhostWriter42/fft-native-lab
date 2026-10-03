#!/usr/bin/env python3
"""statepeek.py -- read memory of a machine-state snapshot written by the lockstep driver (-SnapSave).

  python port/tools/statepeek.py A.state ADDR [LEN]                       hex dump (default 64 bytes) of PS1 RAM / scratchpad / stacks
  python port/tools/statepeek.py A.state ADDR --array COUNT STRIDE OFF:SIZE [OFF:SIZE ...]   one row per element: the listed fields (little endian, hex)
  python port/tools/statepeek.py A.state --sym g_battle_turn_unit_id --symbols port/build/native/ls/symbols_pc.ld   a word by symbol name

Example (the battle units of the first battle): statepeek.py build/states/b10.state 0x801908cc --array 21 0x1c0 0x00:1 0x01:1 0x05:1 0x1b8:1
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from statediff import load   # noqa: E402


def mem_reader(mem, regs):
    def rd(addr, n):
        for lo, size in regs:
            if lo <= addr and addr + n <= lo + size:
                return mem[lo][addr - lo:addr - lo + n]
        raise SystemExit(f'0x{addr:08x}..+{n}: not inside a region of the snapshot')
    return rd


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('state')
    ap.add_argument('addr', nargs='?')
    ap.add_argument('len', nargs='?', default='64')
    ap.add_argument('--array', nargs='+', help='COUNT STRIDE OFF:SIZE ...')
    ap.add_argument('--sym')
    ap.add_argument('--symbols')
    args = ap.parse_args()
    frame, regs, mem = load(args.state)
    rd = mem_reader(mem, regs)
    print(f'state of frame {frame}')
    if args.sym:
        syms = {}
        for line in Path(args.symbols).read_text().splitlines():
            m = re.match(r'(\S+)\s*=\s*(0x[0-9a-fA-F]+);', line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
        a = syms[args.sym]
        print(f'{args.sym} @0x{a:08x} = 0x{struct.unpack("<I", rd(a, 4))[0]:08x}')
        return
    addr = int(args.addr, 0)
    if args.array:
        count, stride = int(args.array[0], 0), int(args.array[1], 0)
        fields = [tuple(int(x, 0) for x in f.split(':')) for f in args.array[2:]]
        for i in range(count):
            base = addr + i * stride
            cells = []
            for off, size in fields:
                v = int.from_bytes(rd(base + off, size), 'little')
                cells.append(f'+0x{off:x}={v:0{size * 2}x}')
            print(f'[{i:2d}] 0x{base:08x}  ' + '  '.join(cells))
        return
    n = int(args.len, 0)
    data = rd(addr, n)
    for i in range(0, n, 16):
        chunk = data[i:i + 16]
        print(f'0x{addr + i:08x}  ' + ' '.join(f'{b:02x}' for b in chunk) + '  ' + ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk))


if __name__ == '__main__':
    main()
