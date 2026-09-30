"""Shared parser for target/*.yaml: the decompiled functions (rows with size) plus the HANDWRITTEN routines.

A handwritten routine is a `kind: handwritten` (or `kind: blocked`: a callable stub the C cannot reproduce, e.g. main_noop_800449ec)
region ({addr, end, kind: handwritten, why}) whose start address also has a bare named row ({addr, name}); the decomp keeps such code
as raw bytes (no C source), but native builds need them as functions: they are returned here with size = end - addr and asm=True.

A yaml file can hold several documents separated by `---` (event.yaml: 11 overlays, effect.yaml: 110), each with its own header
(module / file / lba / size / load ...) and its own rows; documents() splits them and module_functions() parses each one on its own."""
import re
from pathlib import Path

fn_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*size:\s*(\d+),\s*name:\s*([A-Za-z_]\w*)(.*)\}\s*$')
bare_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*name:\s*([A-Za-z_]\w*)\}\s*$')
hw_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*end:\s*(0x[0-9a-fA-F]+),\s*kind:\s*(?:handwritten|blocked)')
header_key = re.compile(r'^(module|file|lba|size|load|source_dir|links):\s*(.*\S)\s*$')


def documents(path):
    """[(name, header, lines)] one per document of the yaml file; name = its `module:` field (or the file stem)."""
    path = Path(path)
    out = []
    for chunk in re.split(r'(?m)^---\s*$', path.read_text()):
        lines = chunk.splitlines()
        hdr = {}
        for l in lines:
            r = header_key.match(l)
            if r:
                hdr.setdefault(r.group(1), r.group(2))
        if not any(fn_row.match(l) or bare_row.match(l) for l in lines) and 'file' not in hdr:
            continue                                   # an empty trailing chunk
        out.append((hdr.get('module', path.stem), hdr, lines))
    return out


def _parse(lines):
    funcs, bare, hw = [], {}, {}
    for line in lines:
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
        # every named entry inside the region is a routine (a region can hold several: wldcore_switch_to_stack / wldcore_restore_previous_stack);
        # `g_` names are data words parked among the code
        starts = sorted(a for a, n in bare.items() if addr <= a < end and (a == addr or not n.startswith('g_')))
        if addr in bare and addr not in starts:
            starts = [addr] + starts
        for i, a in enumerate(starts):
            nxt = starts[i + 1] if i + 1 < len(starts) else end
            if a not in have:
                funcs.append((a, nxt - a, bare[a], True))
                have.add(a)
    return funcs


def module_functions(path, doc=None):
    """[(addr, size, name, asm)] for a module yaml, in file order (functions first, then handwritten routines).
    Multi-document files are parsed document by document; `doc` restricts the result to one document (its module name)."""
    result = []
    for name, hdr, lines in documents(path):
        if doc is not None and name != doc:
            continue
        result.extend(_parse(lines))
    return result
