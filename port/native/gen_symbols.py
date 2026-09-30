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
alias_prefix = ''                                  # --native-prefix=native_ : a function that several overlays define at DIFFERENT addresses is bound to the native definition instead of an address
for a in list(args):
    if a.startswith('--native-prefix='):
        alias_prefix = a.split('=', 1)[1]
        args.remove(a)
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
fn_addrs = {}                                      # function name -> every address it is defined at (effect.yaml repeats a name in many overlays)
for m in mods:
    if with_functions:
        for l in (repo / 'target' / m).read_text().splitlines():
            r = fn_row.match(l)
            if r:
                fn_addrs.setdefault(r.group(2), set()).add(int(r.group(1), 16))
multi = {n for n, a in fn_addrs.items() if len(a) > 1} if alias_prefix else set()
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
        if name in multi:                                                  # one native definition serves every copy: call it directly (each copy's PS1 address still gets a trampoline)
            lines.append(f'{name} = {alias_prefix}{name};')
        else:
            lines.append(f'{name} = 0x{addr:08x};')
# Data symbols that the documents of a multi-document yaml (effect.yaml: one overlay per document) define at DIFFERENT addresses cannot be one link-time constant:
# every overlay has its own copy of such a table. Each document's copy is bound as <name>__<document>; the build renames the references inside the
# object files of that document's functions (boundary.sh, SCOPED) to those names. scoped_syms.txt lists the affected names, fn_docs.txt maps functions to documents.
n_scoped = 0
if alias_prefix:
    doc_names = {}                                                          # data name -> {document: address}
    fn_docs = {}
    for m in mods:
        docs = yamlfuncs.modules(repo / 'target' / m)
        if len(docs) < 2:
            continue
        for stem, hdr, doc_lines in docs:
            for l in doc_lines:
                r = row.match(l)
                if r and r.group(2) not in functions:
                    doc_names.setdefault(r.group(2), {})[stem] = int(r.group(1), 16)
            for _, _, fname, _ in yamlfuncs.parse_lines(doc_lines):
                fn_docs.setdefault(fname, set()).add(stem)
    scoped = {n: d for n, d in doc_names.items() if len(set(d.values())) > 1}
    for n, d in sorted(scoped.items()):
        for stem, addr in sorted(d.items()):
            lines.append(f'{n}__{stem} = 0x{addr:08x};')
            n_scoped += 1
    Path(out).with_name('scoped_syms.txt').write_bytes(('\n'.join(sorted(scoped)) + '\n').encode())
    Path(out).with_name('fn_docs.txt').write_bytes(('\n'.join(f'{f} {next(iter(d))}' for f, d in sorted(fn_docs.items()) if len(d) == 1) + '\n').encode())
Path(out).write_bytes(('\n'.join(lines) + '\n').encode())
print(f'{len(lines)} symbols -> {out} ({clash} name clashes with different addresses, '
      + (f'functions included' if with_functions else f'{dropped} function-address aliases left out') + (f', {len(multi)} multi-address functions bound natively' if multi else '') + (f', {n_scoped} per-overlay data copies' if n_scoped else '') + ')')
