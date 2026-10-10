"""void_ret_scan.py -- find places where code uses the return value of a function the decomp declares `void`.

On the PS1 such a value is whatever the callee left in $v0 (usually the return value of the last function it called); natively it is whatever is left in
eax, which depends on code generation (it changed between Linux GCC and MinGW GCC). Every hit needs a reviewed native patch (native_patches.py) that
returns the value explicitly, or a check that the value is never used.

Looks for (a) casts that call a void function through a pointer type with a return value, `((T (*)(...))name)(...)` with T not void, and
(b) void functions stored in handler tables (arrays of function pointers) whose element type returns a value.

  python port/tools/void_ret_scan.py [--tree port/build/portable]
"""
import argparse
import re
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]


def strip(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tree', default=str(PORT / 'build' / 'portable'))
    a = ap.parse_args()
    tree = Path(a.tree)
    decl = {}                                                                  # function -> declared return type (prototypes and definitions)
    texts = {}
    for p in list((tree / 'include').rglob('*.h')) + list((tree / 'src').rglob('*.c')):
        t = strip(p.read_text(errors='replace'))
        texts[p] = t
        for m in re.finditer(r'(?m)^\s*(?:extern\s+|static\s+)?([A-Za-z_][\w \*]*?)\s*\b([A-Za-z_]\w*)\s*\([^;{)]*\)\s*[;{]', t):
            rt, name = m.group(1).strip(), m.group(2)
            if name in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
                continue
            decl.setdefault(name, rt)
    void_fns = {n for n, rt in decl.items() if re.fullmatch(r'(?:static\s+)?void', rt)}
    hits = []
    cast = re.compile(r'\(\s*\(\s*([A-Za-z_][\w \*]*?)\s*\(\s*\*\s*\)\s*\([^()]*\)\s*\)\s*&?\s*([A-Za-z_]\w*)\s*\)\s*\(')
    for p, t in texts.items():
        if p.suffix != '.c':
            continue
        for m in cast.finditer(t):
            rt, name = m.group(1).strip(), m.group(2)
            if name in void_fns and rt != 'void':
                line = t.count('\n', 0, m.start()) + 1
                # is the value used? (an assignment, a return, a condition or an argument before the cast)
                before = t[max(0, m.start() - 60):m.start()]
                used = bool(re.search(r'(=|return|\(|,|if\s*\(|[<>!]=?)\s*$', before.rstrip()))
                hits.append((p.relative_to(tree).as_posix(), line, name, rt, 'used' if used else 'statement'))
    # (c) direct uses of a void function's result: `x = fn(`, `return fn(`, `if (fn(`, comparisons (possible where the caller sees no prototype:
    # C89 then assumes int)
    direct = re.compile(r'(?:=(?!=)|\breturn\b|\bif\s*\(|\bwhile\s*\(|[!=<>]=|[<>]|&&|\|\|)\s*\(?\s*([A-Za-z_]\w*)\s*\(')
    for p, t in texts.items():
        if p.suffix != '.c':
            continue
        for m in direct.finditer(t):
            name = m.group(1)
            if name in void_fns and not t[m.start():m.end()].lstrip().startswith('return') or (name in void_fns and t[m.start():m.end()].lstrip().startswith('return')):
                if name not in void_fns:
                    continue
                line = t.count('\n', 0, m.start()) + 1
                hits.append((p.relative_to(tree).as_posix(), line, name, 'int (implicit or assigned)', 'used'))
    for h in sorted(set(hits)):
        print(f'{h[0]}:{h[1]}  {h[2]} (void) called as {h[3]}  [{h[4]}]')
    print(f'{len(hits)} call sites')


if __name__ == '__main__':
    main()
