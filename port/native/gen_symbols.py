"""gen_symbols.py REPO OUT.ld [--functions] [module.yaml ...]  -- linker-script assignments `name = 0xADDR;`.

Default (legacy, "native names" scheme): every DATA symbol (rows `- {addr: 0x..., name: X}` with no size/hash) of the given
target/*.yaml files. Names that some module defines as a FUNCTION (a row with size and hash) are left out even when another
module lists them as a bare address row (main.yaml does that for overlay functions it calls): in this scheme a function is
compiled natively under its own name.

With --functions ("PS1-address" scheme): ALSO every function name = its PS1 address. Natively compiled functions are then
linked under `native_<name>` (see boundary.sh / build_run_fuzz.sh) and reached through x86 trampolines written at the PS1
address, so a function pointer taken by native code is the canonical PS1 address, and RAM never holds a native address."""
import re, sys
from pathlib import Path

args = [a for a in sys.argv[1:]]
with_functions = '--functions' in args
if with_functions:
    args.remove('--functions')
data_only = []                                     # --data-only=mod.yaml : modules the build LINKS to but does not include (their data symbols are bound, their functions are not)
for a in list(args):
    if a.startswith('--data-only='):
        data_only.append(a.split('=', 1)[1])
        args.remove(a)
repo, out = Path(args[0]), args[1]
mods = args[2:] or ['main.yaml', 'battle.yaml']
row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*name:\s*([A-Za-z_]\w*)\}\s*$')
fn_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*size:\s*\d+,\s*name:\s*([A-Za-z_]\w*)')
sys.path.insert(0, str(Path(__file__).resolve().parent))
import yamlfuncs
functions = set()                                  # every function name, handwritten routines included
for y in (repo / 'target').glob('*.yaml'):
    for _, _, name, _ in yamlfuncs.module_functions(y):
        functions.add(name)
seen, lines, clash, dropped = {}, [], 0, 0
for m in mods + data_only:
    for l in (repo / 'target' / m).read_text().splitlines():
        r = row.match(l) or (fn_row.match(l) if (with_functions and m not in data_only) else None)
        if not r:
            continue
        addr, name = int(r.group(1), 16), r.group(2)
        if name in functions and (not with_functions or m in data_only):
            dropped += 1
            continue
        if name in seen:
            clash += seen[name] != addr
            continue
        seen[name] = addr
        lines.append(f'{name} = 0x{addr:08x};')
Path(out).write_bytes(('\n'.join(lines) + '\n').encode())
print(f'{len(lines)} symbols -> {out} ({clash} name clashes with different addresses, '
      + (f'functions included' if with_functions else f'{dropped} function-address aliases left out') + ')')
