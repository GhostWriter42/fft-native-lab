"""gen_symbols.py REPO OUT.ld [module.yaml ...]  -- linker-script assignments `name = 0xADDR;` for every DATA symbol
(rows of the form `- {addr: 0x..., name: X}` with no size/hash) in the given target/*.yaml files."""
import re, sys
from pathlib import Path
repo, out = Path(sys.argv[1]), sys.argv[2]
mods = sys.argv[3:] or ['main.yaml', 'battle.yaml']
row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*name:\s*([A-Za-z_]\w*)\}\s*$')
seen, lines, clash = {}, [], 0
for m in mods:
    for l in (repo / 'target' / m).read_text().splitlines():
        r = row.match(l)
        if not r:
            continue
        addr, name = int(r.group(1), 16), r.group(2)
        if name in seen:
            clash += seen[name] != addr
            continue
        seen[name] = addr
        lines.append(f'{name} = 0x{addr:08x};')
Path(out).write_text('\n'.join(lines) + '\n')
print(f'{len(lines)} data symbols -> {out} ({clash} name clashes with different addresses)')
