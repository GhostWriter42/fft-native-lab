#!/usr/bin/env python3
"""closure.py -- transitive call closure of game functions, following identifiers that name a src/**/<name>.c file.

  closure.py <repo-or-snapshot-root> func [func ...]   prints the files reached, which ones use real asm (portify
                                                       report), and the data symbols they reference by name (g_*, D_*).
The regex approach over-approximates slightly (an identifier in a comment counts) but is good enough to size a
native test."""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
start = sys.argv[2:]
index = {}
for p in (root / 'src').rglob('*.c'):
    index.setdefault(p.stem, p)

ident = re.compile(r'\b[A-Za-z_][A-Za-z_0-9]*\b')
seen, order, data = set(), [], set()
stack = list(start)
while stack:
    f = stack.pop()
    if f in seen or f not in index:
        continue
    seen.add(f)
    order.append(f)
    text = index[f].read_text(encoding='utf-8', errors='replace')
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    for name in set(ident.findall(text)):
        if name in index and name not in seen:
            stack.append(name)
        elif re.match(r'^(g_|D_)', name):
            data.add(name)

by_area = {}
for f in order:
    parts = index[f].relative_to(root).parts
    key = parts[1] + ('/' + parts[2] if parts[1] == 'psyq' else '')
    by_area[key] = by_area.get(key, 0) + 1
print(f'{len(order)} functions reached from {start}: ' + ', '.join(f'{k} {v}' for k, v in sorted(by_area.items())))
print(f'{len(data)} distinct data symbols referenced (g_*/D_*)')
if '--list' in sys.argv:
    for f in sorted(order):
        print('  ', index[f].relative_to(root).as_posix())
