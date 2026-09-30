#!/usr/bin/env python3
"""memmap.py REPO -- approximate the PS1 RAM layout from target/*.yaml data symbols.

Sizes are the gap to the next symbol (an over-estimate when padding/unnamed data sits between symbols); regions come from the
module load addresses. Groups symbols by name prefix to show where the mutable state lives."""
import re, sys
from collections import defaultdict
from pathlib import Path

repo = Path(sys.argv[1])
row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*name:\s*([A-Za-z_]\w*)\}\s*$')
funcs = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*size:\s*(\d+),\s*name:\s*(\w+)')
syms = {}
code_ranges = []
for m in ('main', 'battle', 'world', 'wldcore', 'opening'):
    for l in (repo / 'target' / f'{m}.yaml').read_text().splitlines():
        r = row.match(l)
        if r:
            syms.setdefault(m, {})[int(r.group(1), 16)] = r.group(2)
        f = funcs.match(l)
        if f:
            code_ranges.append((int(f.group(1), 16), int(f.group(1), 16) + int(f.group(2)), m))

def prefix(name):
    for p in ('g_battle_unit', 'g_battle_map', 'g_battle_action', 'g_battle_ai', 'g_battle_effect', 'g_battle_gfx',
              'g_battle_menu', 'g_battle_camera', 'g_battle_script', 'g_battle_text', 'g_battle', 'g_world', 'g_wldcore',
              'g_open', 'g_main_gfx', 'g_main_sound', 'g_main_card', 'g_main_item', 'g_main_unit', 'g_main_party',
              'g_main', 'g_psyq', 'g_current_ability', 'g_font', 'g_'):
        if name.startswith(p):
            return p
    return '(other)'

print('module     data symbols   address range')
for m, d in syms.items():
    a = sorted(d)
    print(f'{m:<10} {len(a):>7}     0x{a[0]:08x} - 0x{a[-1]:08x}')

for m in ('battle', 'main'):
    a = sorted(syms[m])
    sizes = defaultdict(int)
    counts = defaultdict(int)
    for i, addr in enumerate(a):
        nxt = a[i + 1] if i + 1 < len(a) else addr + 4
        size = min(nxt - addr, 0x40000)          # cap runaway gaps (code/rodata between symbols)
        p = prefix(syms[m][addr])
        sizes[p] += size
        counts[p] += 1
    print(f'\n== {m}: approximate bytes by symbol prefix (gap to next symbol, capped) ==')
    for p, s in sorted(sizes.items(), key=lambda kv: -kv[1])[:16]:
        print(f'  {p:<22} {counts[p]:>5} symbols  ~{s/1024:>8.1f} KiB')
    print(f'  total covered: ~{sum(sizes.values())/1024:.0f} KiB of {(a[-1]-a[0])/1024:.0f} KiB span')
