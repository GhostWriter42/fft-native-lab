"""gen_fuzz.py REPO OUT_DIR [module.yaml ...]

Generates, for the generic function-level differential fuzzer (harness_fuzz.c):
  OUT_DIR/fuzz_table.c  one row per decompiled game function whose signature is simple enough to call generically:
                        {name, original address, size, native function (weak), return kind, argument kinds}
  OUT_DIR/sym_table.c   every data symbol of the modules, sorted by address (to label RAM addresses in reports)
  OUT_DIR/sdk_ranges.c  the address ranges of the SDK libraries that hardware-touching code lives in (skipped trials)

Signatures come from the C definition in src/ (comments stripped, one definition per file named after the function).
Functions are skipped when a parameter is a struct by value, a function pointer, `...`, or there are more than 10 arguments,
or when the return type is a struct/float/64-bit value. SDK sources (src/psyq) and asm-only functions are not fuzzed."""
import re, sys
from pathlib import Path

args = sys.argv[1:]
sdk_mode = '--sdk' in args                     # include src/psyq reconstructions (libgte and crt are replaced natively, not fuzzed)
if sdk_mode:
    args.remove('--sdk')
prefix = ''                                   # --native-prefix=native_ : natively compiled definitions carry this prefix
for a in list(args):
    if a.startswith('--native-prefix='):
        prefix = a.split('=', 1)[1]
        args.remove(a)
repo, out = Path(args[0]), Path(args[1])
mods = args[2:] or ['main.yaml', 'battle.yaml']
out.mkdir(parents=True, exist_ok=True)

fn_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*size:\s*(\d+),\s*name:\s*([A-Za-z_]\w*)(.*)\}\s*$')
data_row = re.compile(r'^\s*-\s*\{addr:\s*(0x[0-9a-fA-F]+),\s*name:\s*([A-Za-z_]\w*)\}\s*$')
lib_row = re.compile(r'^\s*-\s*\{id:\s*(\w+),\s*addr:\s*(0x[0-9a-fA-F]+),\s*end:\s*(0x[0-9a-fA-F]+)')

# ---- kinds ------------------------------------------------------------------------------------------------------
S8, U8, S16, U16, S32, U32, PTR = 1, 2, 3, 4, 5, 6, 7
scalar_types = {
    's8': S8, 'u8': U8, 's16': S16, 'u16': U16, 's32': S32, 'u32': U32,
    'int': S32, 'long': S32, 'short': S16, 'char': U8,          # -funsigned-char: plain char is unsigned
    'unsigned': U32, 'signed': S32,
}
qualifiers = {'const', 'volatile', 'register', 'static', 'extern', 'inline'}


def classify(type_text):
    """kind code for a C type spelled as text (no declarator name), or None when it cannot be passed generically."""
    t = type_text.strip()
    if '*' in t or '[' in t:
        return PTR
    words = [w for w in re.findall(r'[A-Za-z_]\w*', t) if w not in qualifiers]
    if not words:
        return None
    if words == ['void']:
        return 0
    if words[0] in ('struct', 'union'):
        return None
    if words[0] == 'enum':
        return S32
    if words[0] == 'unsigned':
        rest = words[1:]
        if not rest or rest == ['int'] or rest == ['long']:
            return U32
        if rest == ['short']:
            return U16
        if rest == ['char']:
            return U8
        return None
    if words[0] == 'signed':
        rest = words[1:]
        if not rest or rest == ['int'] or rest == ['long']:
            return S32
        if rest == ['short']:
            return S16
        if rest == ['char']:
            return S8
        return None
    if len(words) == 1:
        w = words[0]
        if w in scalar_types:
            return scalar_types[w]
        if w.endswith('_e'):                      # enums are typedef'd as name_e in this codebase
            return S32
    return None


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    return text


def split_params(s):
    parts, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


def param_type(p):
    """type text of a parameter (drop the declarator name, keep pointer stars / array brackets)"""
    if '(' in p:
        return None                                # function pointer
    if p == '...':
        return None
    m = re.match(r'^(.*?)(\w+)\s*(\[[^\]]*\])*\s*$', p, flags=re.S)
    if not m:
        return None
    base = m.group(1)
    name = m.group(2)
    # a lone type with no name (e.g. "void", "s32") leaves an empty base
    if not base.strip():
        return name
    return base + (m.group(3) or '')


def parse_signature(name, path):
    text = strip_comments(path.read_text(errors='replace'))
    m = re.search(r'(?m)^([A-Za-z_][\w \t\*]*?)[ \t\*]\b' + re.escape(name) + r'\s*\(', text)
    if not m:
        return None, 'no definition'
    ret_text = m.group(1) + ('*' if text[m.end(1)] == '*' else '')
    # the char after group(1) may be a '*' that belongs to the return type
    start = m.end()
    depth, i = 1, start
    while i < len(text) and depth:
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
        i += 1
    params_text = text[start:i - 1]
    rest = text[i:].lstrip()
    if not rest.startswith('{'):
        return None, 'not a definition'
    ret = classify(ret_text)
    if ret is None:
        return None, 'return type'
    params = split_params(params_text) if params_text.strip() not in ('', 'void') else []
    kinds = []
    for p in params:
        pt = param_type(p)
        if pt is None:
            return None, 'parameter'
        k = classify(pt)
        if k is None or k == 0:
            return None, 'parameter type'
        kinds.append(k)
    if len(kinds) > 10:
        return None, 'too many arguments'
    return (ret, kinds), None


# ---- collect ---------------------------------------------------------------------------------------------------------
src_by_name = {}
for p in (repo / 'src').rglob('*.c'):
    src_by_name.setdefault(p.stem, []).append(p)

funcs, data, libs = [], [], {}
seen = set()
for m in mods:
    in_libs = False
    for line in (repo / 'target' / m).read_text().splitlines():
        if line.startswith('libraries:'):
            in_libs = True
            continue
        if line.startswith('functions:') or line.startswith('symbols:'):
            in_libs = False
        if in_libs:
            r = lib_row.match(line)
            if r and m == 'main.yaml':
                libs[r.group(1)] = (int(r.group(2), 16), int(r.group(3), 16))
            continue
        r = fn_row.match(line)
        if r:
            name = r.group(3)
            if name in seen:
                continue
            seen.add(name)
            funcs.append((int(r.group(1), 16), int(r.group(2)), name, r.group(4)))
            continue
        r = data_row.match(line)
        if r:
            data.append((int(r.group(1), 16), r.group(2)))

skipped = {}
rows = []
for addr, size, name, extra in funcs:
    if 'asm:' in extra:
        skipped['asm-only'] = skipped.get('asm-only', 0) + 1
        continue
    cands = [p for p in src_by_name.get(name, []) if 'psyq' not in p.parts or (sdk_mode and not ({'libgte', 'crt'} & set(p.parts)))]
    if not cands:
        skipped['no game source (SDK/asm)'] = skipped.get('no game source (SDK/asm)', 0) + 1
        continue
    sig, why = parse_signature(name, cands[0])
    if sig is None:
        skipped[why] = skipped.get(why, 0) + 1
        continue
    rows.append((addr, size, name, sig[0], sig[1]))

lines = ['/* generated by gen_fuzz.py */',
         'struct fuzz_fn { const char* name; unsigned int addr; unsigned int size; void* nat; unsigned char ret, nargs, kind[10]; };']
for _, _, n, _, _ in rows:
    lines.append(f'extern char {prefix}{n}[] __attribute__((weak));')
lines.append('const struct fuzz_fn g_fuzz_fns[] = {')
for addr, size, name, ret, kinds in rows:
    ks = ', '.join(str(k) for k in kinds + [0] * (10 - len(kinds)))
    lines.append(f'    {{ "{name}", 0x{addr:08x}u, {size}u, (void*){prefix}{name}, {ret}, {len(kinds)}, {{ {ks} }} }},')
lines.append('    { 0, 0, 0, 0, 0, 0, { 0 } }\n};')
lines.append(f'const int g_fuzz_fn_count = {len(rows)};')
(out / 'fuzz_table.c').write_bytes(('\n'.join(lines) + '\n').encode())

data.sort()
sl = ['/* generated by gen_fuzz.py */', 'struct sym { unsigned int addr; const char* name; };', 'const struct sym g_syms[] = {']
for a, n in data:
    sl.append(f'    {{ 0x{a:08x}u, "{n}" }},')
sl.append('    { 0, 0 }\n};')
sl.append(f'const int g_sym_count = {len(data)};')
(out / 'sym_table.c').write_bytes(('\n'.join(sl) + '\n').encode())

sdk = [] if sdk_mode else [(a, e, i) for i, (a, e) in libs.items() if i in ('libspu', 'libetc', 'libcd', 'libgpu', 'libcard')]
sdk.sort()
rl = ['/* generated by gen_fuzz.py: SDK library ranges (hardware-touching code) */',
      'struct sdk_range { unsigned int lo, hi; const char* id; };', 'const struct sdk_range g_sdk_ranges[] = {']
for a, e, i in sdk:
    rl.append(f'    {{ 0x{a:08x}u, 0x{e:08x}u, "{i}" }},')
rl.append('    { 0, 0, 0 }\n};')
rl.append(f'const int g_sdk_range_count = {len(sdk)};')
(out / 'sdk_ranges.c').write_bytes(('\n'.join(rl) + '\n').encode())

# ---- global variables worth seeding before every trial (pointers -> valid buffers, scalars -> random values) -------------------
sym_addr = {n: a for a, n in data}
skip_seed = re.compile(r'(register|dma|heap|overlay_load|psyq|spu_|_regs?$)')   # hardware pointers and SDK state keep their real values
ptr_re = re.compile(r'^extern\s+(?:const\s+)?(?:struct\s+)?[\w ]+?\s*\*+\s*(g_\w+)\s*;', re.M)
scalar_re = re.compile(r'^extern\s+(?:volatile\s+)?(s8|u8|s16|u16|s32|u32|int|short|char)\s+(g_\w+)\s*;', re.M)
ptr_names, scalar_names = {}, {}
for h in list((repo / 'include').rglob('*.h')):
    t = strip_comments(h.read_text(errors='replace'))
    for m in ptr_re.finditer(t):
        if m.group(1) in sym_addr and not skip_seed.search(m.group(1)):
            ptr_names[m.group(1)] = sym_addr[m.group(1)]
    for m in scalar_re.finditer(t):
        if m.group(2) in sym_addr and not skip_seed.search(m.group(2)):
            scalar_names[m.group(2)] = (sym_addr[m.group(2)], scalar_types.get(m.group(1), S32))
gl = ['/* generated by gen_fuzz.py: globals seeded before every trial */',
      'const unsigned int g_ptr_globals[] = {']
for n, a in sorted(ptr_names.items(), key=lambda kv: kv[1]):
    gl.append(f'    0x{a:08x}u, /* {n} */')
gl.append('    0\n};')
gl.append(f'const int g_ptr_global_count = {len(ptr_names)};')
gl.append('struct scalar_global { unsigned int addr; unsigned char kind; };')
gl.append('const struct scalar_global g_scalar_globals[] = {')
for n, (a, k) in sorted(scalar_names.items(), key=lambda kv: kv[1][0]):
    gl.append(f'    {{ 0x{a:08x}u, {k} }}, /* {n} */')
gl.append('    { 0, 0 }\n};')
gl.append(f'const int g_scalar_global_count = {len(scalar_names)};')
(out / 'seed_globals.c').write_bytes(('\n'.join(gl) + '\n').encode())

print(f'{len(ptr_names)} pointer globals and {len(scalar_names)} scalar globals to seed')
print(f'{len(rows)} of {len(funcs)} functions can be fuzzed generically; skipped: '
      + ', '.join(f'{k} {v}' for k, v in sorted(skipped.items())) + f'; {len(data)} data symbols; {len(sdk)} SDK ranges')
