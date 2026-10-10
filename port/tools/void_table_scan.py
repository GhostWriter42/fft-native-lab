"""void_table_scan.py -- handler tables (arrays of function pointers that return a value) whose entries are functions the decomp declares `void`.

A table's contents are data from the disc, so they are read from machine-state snapshots (the overlay that owns the table must be loaded in one of them).
Such an entry returns whatever the void function left in $v0 on the PS1 and whatever is in eax natively (see void_ret_scan.py).

  python port/tools/void_table_scan.py STATE [STATE ...]
"""
import re
import struct
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT / 'tools'))
from statediff import load   # noqa: E402


def strip(t):
    return re.sub(r'//[^\n]*', ' ', re.sub(r'/\*.*?\*/', ' ', t, flags=re.S))


def main():
    tree = PORT / 'build' / 'portable'
    texts = [strip(p.read_text(errors='replace')) for p in list((tree / 'include').rglob('*.h')) + list((tree / 'src').rglob('*.c'))]
    decl = {}
    for t in texts:
        for m in re.finditer(r'(?m)^\s*(?:extern\s+|static\s+)?([A-Za-z_][\w \*]*?)\s*\b([A-Za-z_]\w*)\s*\([^;{)]*\)\s*[;{]', t):
            decl.setdefault(m.group(2), m.group(1).strip())
    tables = {}                                                                # name -> return type of the element
    for t in texts:
        for m in re.finditer(r'extern\s+([A-Za-z_][\w \*]*?)\s*\(\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)\s*\[[^\]]*\]\s*\)\s*\(', t):
            tables[m.group(2)] = m.group(1).strip()
    typedefs = {}
    for t in texts:
        for m in re.finditer(r'typedef\s+([A-Za-z_][\w \*]*?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(', t):
            typedefs[m.group(2)] = m.group(1).strip()
        for m in re.finditer(r'extern\s+([A-Za-z_]\w*)\s+(?:const\s+)?([A-Za-z_]\w*)\s*\[[^\]]*\]\s*;', t):
            if m.group(1) in typedefs:
                tables[m.group(2)] = typedefs[m.group(1)]
    syms, fns = {}, {}
    for y in (PORT.parent / 'fft_decomp' / 'target').glob('*.yaml'):
        for line in y.read_text().splitlines():
            m = re.search(r'addr: (0x[0-9a-f]+)(?:, size: (\d+))?, name: (\w+)', line)
            if m:
                if m.group(2):
                    fns.setdefault(int(m.group(1), 16), m.group(3))
                else:
                    syms.setdefault(m.group(3), int(m.group(1), 16))
    states = [load(s) for s in sys.argv[1:]]
    found = 0
    for name, rt in sorted(tables.items()):
        if rt == 'void' or name not in syms:
            continue
        base = syms[name]
        for _, regs, mem in states:
            ram = mem.get(0x80000000)
            if ram is None:
                continue
            bad, n = [], 0
            for i in range(512):
                off = base - 0x80000000 + 4 * i
                if off + 4 > len(ram):
                    break
                v = struct.unpack_from('<I', ram, off)[0]
                if v not in fns:
                    break
                n += 1
                f = fns[v]
                if re.fullmatch(r'(?:static\s+)?void', decl.get(f, '')):
                    bad.append((i, f))
            if n:
                for i, f in bad:
                    print(f'{name}[{i}] = {f}: declared void, the table expects {rt}')
                    found += 1
                break
    print(f'{found} void entries in value-returning handler tables ({len(tables)} tables declared)')


if __name__ == '__main__':
    main()
