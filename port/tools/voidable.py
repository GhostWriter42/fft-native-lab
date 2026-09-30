#!/usr/bin/env python3
"""voidable.py REPO -- find non-void functions that never return a value, and whether any caller uses the result.

A function whose every path ends in bare `return;` (or falls off the end) but is declared `s32 f(...)`/`u8 f(...)` returns
whatever was left in $v0 on the PS1 -- undefined natively. If no caller consumes the value, retyping to `void` is
byte-neutral. Output: candidates (safe), and unsafe ones with the caller line that uses the value."""
import re, subprocess, sys
from pathlib import Path

repo = Path(sys.argv[1])
files = subprocess.check_output(['git', '-C', str(repo), 'ls-files', 'src/*.c'], text=True).split()
texts = {f: (repo / f).read_text(encoding='utf-8', errors='replace') for f in files}

def strip(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)

sig = re.compile(r'^(?P<ret>(?:const\s+)?[A-Za-z_]\w*(?:\s*\*+)?)\s+(?P<name>[A-Za-z_]\w*)\s*\((?P<args>[^;{)]*)\)\s*\{', re.M)
cands, uses = {}, {}
for f, t in texts.items():
    st = strip(t)
    stem = Path(f).stem
    for m in sig.finditer(st):
        if m['name'] != stem or m['ret'].strip() == 'void':
            continue
        # body: from '{' to the matching '}'
        i, depth = m.end() - 1, 0
        for j in range(i, len(st)):
            if st[j] == '{':
                depth += 1
            elif st[j] == '}':
                depth -= 1
                if depth == 0:
                    body = st[i:j + 1]
                    break
        else:
            continue
        rets = re.findall(r'\breturn\b\s*([^;]*);', body)
        if any(r.strip() for r in rets):
            continue                      # returns a value somewhere
        if re.search(r'\bgoto\b', body) and False:
            continue
        cands[m['name']] = (f, m['ret'].strip(), len(rets))

# does any caller use the value? (anything other than a bare statement `name(...);`)
call = {name: re.compile(r'(?<![\w.>])' + re.escape(name) + r'\s*\(') for name in cands}
for f, t in texts.items():
    st = strip(t)
    for name, rx in call.items():
        if name == Path(f).stem and f == cands[name][0]:
            pass
        for m in rx.finditer(st):
            s = st.rfind('\n', 0, m.start()) + 1
            prefix = st[s:m.start()].strip()
            # a bare call statement: the call starts the statement (prefix empty or ends with ; { } or )/else)
            if prefix == '' or prefix.endswith((';', '{', '}', 'else')) or re.fullmatch(r'(if|while|for)\s*\(.*\)', prefix):
                # `if (x) name(...)`: prefix ends with ')' and starts with if/while/for => still a statement
                continue
            # a definition/declaration line (`s32 name(`) or a cast-call `((void(*)...)name)(` -- record as use
            if re.fullmatch(r'[A-Za-z_][\w\s\*]*', prefix) and Path(f).stem == name:
                continue
            uses.setdefault(name, []).append(f'{f}: {st[s:m.start() + 60].strip()[:100]}')

safe = sorted(n for n in cands if n not in uses)
unsafe = sorted(n for n in cands if n in uses)
print(f'non-void functions with no value-returning `return`: {len(cands)}')
print(f'  no caller appears to use the value (voidable): {len(safe)}')
print(f'  a caller appears to use the value:            {len(unsafe)}')
by_ret = {}
for n in safe:
    by_ret[cands[n][1]] = by_ret.get(cands[n][1], 0) + 1
print('  voidable by declared return type:', dict(sorted(by_ret.items(), key=lambda kv: -kv[1])))
Path(sys.argv[2] if len(sys.argv) > 2 else 'voidable.txt').write_text(
    '\n'.join(f'{cands[n][0]} {cands[n][1]} {n}' for n in safe) + '\n')
print('  first unsafe examples:')
for n in unsafe[:6]:
    print('   ', n, '->', uses[n][0])
