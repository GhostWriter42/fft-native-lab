"""gen_symbols.py REPO OUT.ld [module.yaml ...]  -- linker-script assignments `name = 0xADDR;` for every DATA symbol
(rows of the form `- {addr: 0x..., name: X}` with no size/hash) in the given target/*.yaml files.

Names that some module defines as a FUNCTION (a row with size and hash) are left out even when another module lists them as a
bare address row (main.yaml does that for overlay functions it calls): a function must be compiled natively, not linked at its
PS1 address."""
import re, sys
from pathlib import Path
repo, out = Path(sys.argv[1]), sys.argv[2]
mods = sys.argv[3:] or ['main.yaml', 'battle.yaml']
row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*name:\s*([A-Za-z_]\w*)\}\s*$')
fn_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*size:\s*\d+,\s*name:\s*([A-Za-z_]\w*)')
functions = set()
for y in (repo / 'target').glob('*.yaml'):
    for l in y.read_text().splitlines():
        r = fn_row.match(l)
        if r:
            functions.add(r.group(2))
seen, lines, clash, dropped = {}, [], 0, 0
for m in mods:
    for l in (repo / 'target' / m).read_text().splitlines():
        r = row.match(l)
        if not r:
            continue
        addr, name = int(r.group(1), 16), r.group(2)
        if name in functions:
            dropped += 1
            continue
        if name in seen:
            clash += seen[name] != addr
            continue
        seen[name] = addr
        lines.append(f'{name} = 0x{addr:08x};')
Path(out).write_text('\n'.join(lines) + '\n')
print(f'{len(lines)} data symbols -> {out} ({clash} name clashes with different addresses, {dropped} function-address aliases left out)')
