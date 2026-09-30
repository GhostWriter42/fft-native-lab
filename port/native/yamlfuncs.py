"""Shared parser for target/*.yaml: the decompiled functions (rows with size) plus the HANDWRITTEN routines.

A handwritten routine is a `kind: handwritten` region ({addr, end, kind: handwritten, why}) whose start address also has a bare
named row ({addr, name}); the decomp keeps such code as raw bytes (no C source), but native builds need them as functions:
they are returned here with size = end - addr and asm=True."""
import re
from pathlib import Path

fn_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*size:\s*(\d+),\s*name:\s*([A-Za-z_]\w*)(.*)\}\s*$')
bare_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*name:\s*([A-Za-z_]\w*)\}\s*$')
hw_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*end:\s*(0x[0-9a-fA-F]+),\s*kind:\s*handwritten')


def module_functions(path):
    """[(addr, size, name, asm)] for one module yaml, in file order (functions first, then handwritten routines)."""
    funcs, bare, hw = [], {}, {}
    for line in Path(path).read_text().splitlines():
        r = fn_row.match(line)
        if r:
            funcs.append((int(r.group(1), 16), int(r.group(2)), r.group(3), 'asm:' in r.group(4)))
            continue
        r = bare_row.match(line)
        if r:
            bare.setdefault(int(r.group(1), 16), r.group(2))
            continue
        r = hw_row.match(line)
        if r:
            hw[int(r.group(1), 16)] = int(r.group(2), 16)
    have = {a for a, _, _, _ in funcs}
    for addr, end in sorted(hw.items()):
        if addr in bare and addr not in have:
            funcs.append((addr, end - addr, bare[addr], True))
    return funcs
