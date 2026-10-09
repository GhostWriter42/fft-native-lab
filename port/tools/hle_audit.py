"""hle_audit.py -- which state does a platform-layer (HLE) replacement have to keep?

The platform layer takes over SDK functions (port/build/native/hle_natives.txt). Both lockstep machines share it, so the soaks cannot see a
replacement that forgets state the real library keeps (the flashing name-entry screen: PutDrawEnv / ResetGraph). This lists, for every
replaced function, what the real library code (the decomp's C, including the SDK functions it calls) WRITES that somebody else READS:

  * globals written (assignment, ++/--, memcpy/memset destination, address passed to a callee) and read by code the HLE does NOT replace;
  * memory written through pointer parameters (output arguments: the caller's structures).

  python port/tools/hle_audit.py [--all]      (writes port/build/hle_audit.txt; --all also lists globals nobody else reads)

A heuristic text scan (no C parser): it over-reports; every entry is checked by hand against hle/*.c.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'fft_decomp' / 'src'
NATIVES = ROOT / 'port' / 'build' / 'native' / 'hle_natives.txt'
SYMS = ROOT / 'port' / 'build' / 'native' / 'ls' / 'symbols_pc.ld'
OUT = ROOT / 'port' / 'build' / 'hle_audit.txt'

FUNC_DEF = re.compile(r'^[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\(([^)]*)\)\s*\{', re.M)
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
KEYWORDS = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'do'}


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', t)


def bodies():
    """{function name: (body text, [parameter names], file)} for every C function of the decomp"""
    out = {}
    for f in SRC.rglob('*.c'):
        t = strip_comments(f.read_text(encoding='utf-8', errors='replace'))
        for m in FUNC_DEF.finditer(t):
            name = m.group(1)
            if name in KEYWORDS:
                continue
            depth, i = 1, m.end()
            while i < len(t) and depth:
                depth += {'{': 1, '}': -1}.get(t[i], 0)
                i += 1
            params = [p.strip().split()[-1].lstrip('*') for p in m.group(2).split(',') if p.strip() and p.strip() != 'void']
            params = [re.sub(r'\[.*', '', p) for p in params]
            out.setdefault(name, (t[m.end():i - 1], params, f.relative_to(SRC).as_posix()))
    return out


def globals_list():
    names = set()
    for line in SYMS.read_text(encoding='utf-8').splitlines():
        m = re.match(r'\s*([A-Za-z_]\w*)\s*=\s*0x', line)
        if m:
            names.add(m.group(1))
    return names


def writes(body, gl):
    """globals written by this body (heuristic)"""
    w = set()
    for m in re.finditer(r'\b([A-Za-z_]\w*)\b((?:\s*(?:\[[^\]]*\]|\.\w+|->\w+))*)\s*(?:[-+*/|&^]|<<|>>)?=(?!=)', body):
        if m.group(1) in gl:
            w.add(m.group(1))
    for m in re.finditer(r'(?:\+\+|--)\s*\b([A-Za-z_]\w*)|\b([A-Za-z_]\w*)\b(?:\s*(?:\[[^\]]*\]|\.\w+|->\w+))*\s*(?:\+\+|--)', body):
        n = m.group(1) or m.group(2)
        if n in gl:
            w.add(n)
    for m in re.finditer(r'&\s*\(?\s*([A-Za-z_]\w*)', body):              # address taken: the callee may write it
        if m.group(1) in gl:
            w.add(m.group(1))
    return w


def reads(body, gl):
    return {n for n in re.findall(r'\b[A-Za-z_]\w*\b', body) if n in gl}


def param_writes(body, params):
    """pointer parameters written through (output arguments)"""
    hit = set()
    for p in params:
        if re.search(r'\b' + re.escape(p) + r'\s*(?:->\w+|\[[^\]]*\])(?:\s*(?:\[[^\]]*\]|\.\w+|->\w+))*\s*(?:[-+*/|&^]|<<|>>)?=(?!=)', body) \
                or re.search(r'\*\s*\(?\s*' + re.escape(p) + r'\b[^;=]*?(?<![=!<>])=(?!=)', body):
            hit.add(p)
    return hit


def main():
    show_all = '--all' in sys.argv
    replaced = {l.strip()[len('native_'):] for l in NATIVES.read_text().splitlines() if l.strip()}
    fn = bodies()
    gl = globals_list()
    # closure: the replaced functions plus every decomp function they call that is not game code the HLE leaves alone (SDK internals)
    def callees(name, seen):
        if name in seen or name not in fn:
            return
        seen.add(name)
        for c in CALL.findall(fn[name][0]):
            if c in fn and fn[c][2].startswith('psyq/'):
                callees(c, seen)
    hidden = set()                                                              # SDK internals reached only through replaced functions: never run natively
    for name in replaced:
        callees(name, hidden)
    called_by_live = set()                                                      # ... unless code that does run natively calls them too
    for name, (body, _, f) in fn.items():
        if name not in hidden:
            called_by_live |= {c for c in CALL.findall(body) if c in hidden and c not in replaced}
    hidden -= called_by_live
    readers = {}                                                                # global -> functions that run natively and read it
    for name, (body, _, _) in fn.items():
        if name in replaced or name in hidden:
            continue
        for g in reads(body, gl):
            readers.setdefault(g, set()).add(name)
    lines = []
    for name in sorted(replaced):
        if name not in fn:
            continue
        closure = set()
        callees(name, closure)
        wr, outp = set(), set()
        for c in closure:
            wr |= writes(fn[c][0], gl)
        outp = param_writes(fn[name][0], fn[name][1])
        for c in CALL.findall(fn[name][0]):                                     # a parameter passed straight on to a callee that writes ITS parameter
            if c in fn and c in closure:
                outp |= {p for p in fn[name][1] if re.search(re.escape(c) + r'\s*\([^;]*\b' + re.escape(p) + r'\b', fn[name][0])
                         and param_writes(fn[c][0], fn[c][1])}
        shared = sorted(g for g in wr if readers.get(g) and (g not in replaced))
        quiet = sorted(g for g in wr if not readers.get(g))
        if not shared and not outp and not (show_all and quiet):
            continue
        lines.append(f'{name}  ({fn[name][2]}; {len(closure)} SDK functions in its closure)')
        for g in shared:
            rd = sorted(readers[g])
            lines.append(f'    writes {g:48s} read by {len(rd)}: {", ".join(rd[:6])}{" ..." if len(rd) > 6 else ""}')
        if outp:
            lines.append(f'    writes through parameter(s): {", ".join(sorted(outp))}')
        if show_all and quiet:
            lines.append(f'    (also writes, nobody else reads: {", ".join(quiet)})')
    OUT.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    n = sum(1 for l in lines if not l.startswith(' '))
    print(f'{len(replaced)} replaced functions, {n} with state to check -> {OUT}')


if __name__ == '__main__':
    main()
