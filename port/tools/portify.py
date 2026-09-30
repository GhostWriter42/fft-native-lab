#!/usr/bin/env python3
"""portify.py -- generate portable copies of the fft_decomp sources for a native (modern compiler) build.

The matching decomp keeps GCC 2.6.3/MIPS-only constructs so that the bytes match the original disc. A native
port cannot use them, but they carry no meaning for a native build. This tool leaves the repo untouched and
writes transformed copies under an output directory:

  1. `register T x __asm__("$N");`         -> `T x;`         (MIPS register pins; also file-scope ones in headers)
  2. `__asm__ [volatile] ("" ...);`        -> removed        (empty-template scheduling barriers / ties / clobbers)
  3. `extern T volatile g;` (file-local "volatile views" of a global declared without volatile in a header)
                                           -> `#define g (*(T volatile*)&g)`   (same semantics, valid C)
  4. asm with a NON-empty template         -> left as is and listed in the report (needs a real port)
  5. `((s32 (*)(void))f)()` where f is declared to return s8/u8/s16/u16
                                           -> `((s32)f())`   (the retail callee returns its value already sign/zero extended in $v0, which the
                                              cast call consumes as 32 bits; a native callee leaves the upper bits undefined -- see narrow_return_calls)

Usage:  portify.py <repo-or-snapshot-root> <out-dir> [--report FILE]
Only the standard library is needed. Comments and string/char literals are respected (masked before matching).
"""
import argparse
import os
import re
import sys
from collections import Counter
from pathlib import Path

ASM_TOKEN = re.compile(r'\b(?:__asm__|__asm|asm)\b')
REGISTER_TOKEN = re.compile(r'\bregister\b')
VOLATILE_VIEW = re.compile(
    r'^(?P<ind>[ \t]*)extern\s+(?P<decl>[^;=()]*?\bvolatile\b[^;=()]*?)\s*;[ \t]*(?P<tail>/\*.*\*/)?[ \t]*$')
REGISTER_NAME = re.compile(r'^\$?[A-Za-z0-9]+$')


def mask(text):
    """Return `text` with comments, string literals and char literals blanked out (same length/newlines)."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif c in '"\'':
            q = c
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            for k in range(i + 1, j - 1):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        else:
            i += 1
    return ''.join(out)


def match_paren(masked, open_idx):
    """Index just after the ')' matching the '(' at open_idx, or -1."""
    depth = 0
    for k in range(open_idx, len(masked)):
        ch = masked[k]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                return k + 1
    return -1


def string_literals(text, start):
    """Concatenate adjacent string literals starting at/after `start`; return (value, end_index)."""
    m = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')
    val, pos = '', start
    while True:
        mm = m.match(text, pos)
        if not mm:
            return val, pos
        val += mm.group(1)
        pos = mm.end()


def split_top(masked, start, end, sep):
    """Split masked[start:end] on `sep` at paren/bracket depth 0; returns (begin, end) index pairs."""
    parts, depth, b = [], 0, start
    for k in range(start, end):
        ch = masked[k]
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == sep and depth == 0:
            parts.append((b, k))
            b = k + 1
    parts.append((b, end))
    return parts


OPERAND = re.compile(r'^\s*"(?P<c>[^"]*)"\s*\((?P<e>.*)\)\s*$', re.S)


def asm_data_flow(text, masked, paren_open, paren_end):
    """For an empty-template asm: return the C assignments it performs (a list of 'out = in;' strings) and a flag
    saying whether it has an output that is set by nothing (output-only). A self-tie ("=r"(x) : "0"(x)) or a pure
    input/memory barrier performs no data flow, so it can simply be deleted."""
    inner_start, inner_end = paren_open + 1, paren_end - 1
    sections = split_top(masked, inner_start, inner_end, ':')          # template, outputs, inputs, clobbers
    def operands(idx):
        if idx >= len(sections):
            return []
        b, e = sections[idx]
        if not masked[b:e].strip():
            return []
        res = []
        for pb, pe in split_top(masked, b, e, ','):
            m = OPERAND.match(text[pb:pe])
            if m:
                res.append((m.group('c'), m.group('e').strip()))
        return res
    outs, ins = operands(1), operands(2)
    assigns, output_only = [], False
    for i, (c, o) in enumerate(outs):
        if not c.startswith(('=', '+')):
            continue
        tied = [e for (ic, e) in ins if ic.strip() == str(i)]
        if tied:
            if tied[0] != o:
                assigns.append(f'{o} = {tied[0]};')
        elif c.startswith('='):
            output_only = True
    return assigns, output_only


def parse_asm(text, masked, tok_end):
    """Parse what follows an asm token. Returns (kind, template, paren_start, paren_end) or None."""
    k = tok_end
    while masked[k:k + 1].isspace():
        k += 1
    for q in ('volatile', '__volatile__', 'goto'):
        if masked.startswith(q, k) and not (masked[k + len(q):k + len(q) + 1].isalnum() or masked[k + len(q):k + len(q) + 1] == '_'):
            k += len(q)
            while masked[k:k + 1].isspace():
                k += 1
    if masked[k:k + 1] != '(':
        return None
    end = match_paren(masked, k)
    if end < 0:
        return None
    template, _ = string_literals(text, k + 1)
    return template, k, end


HEADER_EXTERN = re.compile(r'\bextern\b([^;(){}]*?)\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*;')


INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)


class HeaderIndex:
    """Per-header extern declarations plus include edges, so a file's view can be checked against the headers
    it really includes (transitively), not against every header in the tree."""

    def __init__(self, root):
        self.root = root
        self.direct = {}     # header rel path (relative to include/) -> {name: has_volatile}
        self.edges = {}      # header -> [included header, ...]
        for p in sorted((root / 'include').rglob('*.h')):
            rel = p.relative_to(root / 'include').as_posix()
            text = p.read_bytes().decode('utf-8')
            decls = {}
            for m in HEADER_EXTERN.finditer(mask(text)):
                name, has_vol = m.group(2), 'volatile' in m.group(1)
                decls[name] = decls.get(name, False) or has_vol
            self.direct[rel] = decls
            self.edges[rel] = INCLUDE.findall(text)
        self._closure = {}

    def closure_of(self, includes):
        """Merged {name: has_volatile} over the transitive closure of the given include names."""
        seen, stack, merged = set(), list(includes), {}
        while stack:
            h = stack.pop()
            if h in seen or h not in self.direct:
                continue
            seen.add(h)
            for name, vol in self.direct[h].items():
                merged[name] = merged.get(name, False) or vol
            stack.extend(self.edges.get(h, []))
        return merged


REAL_ASM = re.compile(r'(?:__asm__|__asm|\basm\b)\s*(?:volatile|__volatile__)?\s*\(\s*"[^"$]')
DEFINE_LINE = re.compile(r'^[ \t]*#[ \t]*define\b')


def drop_asm_macros(text, rel, report):
    """Remove file-local `#define` blocks (with line continuations) whose body contains real, instruction-emitting asm.
    They would shadow the native shim's macro of the same name; the shim must provide them (report lists each)."""
    lines, out, i = text.split('\n'), [], 0
    while i < len(lines):
        if DEFINE_LINE.match(lines[i]):
            j = i
            while lines[j].rstrip().endswith('\\') and j + 1 < len(lines):
                j += 1
            block = '\n'.join(lines[i:j + 1])
            if REAL_ASM.search(block):
                nm = re.match(r'^[ \t]*#[ \t]*define[ \t]+(\w+)', lines[i])
                report['asm_macros_dropped'].append((rel, i + 1, nm.group(1) if nm else '?'))
                i = j + 1
                continue
            out.extend(lines[i:j + 1])
            i = j + 1
            continue
        out.append(lines[i])
        i += 1
    return '\n'.join(out)


def transform(text, rel, report):
    if rel.endswith('.c'):
        text = drop_asm_macros(text, rel, report)
    masked = mask(text)
    edits = []          # (start, end, replacement)
    consumed = []       # spans already handled (pins)

    # --- 1. register pins ------------------------------------------------------------------------------
    for m in REGISTER_TOKEN.finditer(masked):
        s = m.start()
        # end of the declaration: first ';' at depth 0, stopping at ')' / '{' / ',' at depth 0 (parameter)
        depth, k, end = 0, m.end(), None
        while k < len(masked):
            ch = masked[k]
            if ch in '([':
                depth += 1
            elif ch in ')]':
                if depth == 0:
                    end = k
                    break
                depth -= 1
            elif ch == '{' and depth == 0:
                end = k
                break
            elif ch == ',' and depth == 0:
                end = k
                break
            elif ch == ';' and depth == 0:
                end = k
                break
            k += 1
        if end is None:
            continue
        seg = masked[m.end():end]
        pin = None
        for a in ASM_TOKEN.finditer(seg):
            parsed = parse_asm(text, masked, m.end() + a.end())
            if parsed:
                template, po, pe = parsed
                if REGISTER_NAME.match(template.strip()):
                    ws = m.end() + a.start()
                    while ws > 0 and masked[ws - 1] in ' \t':
                        ws -= 1
                    pin = (ws, pe)
                    break
        edits.append((s, m.end() + (1 if masked[m.end():m.end() + 1] == ' ' else 0), ''))   # drop `register `
        if pin:
            # $0 is the hardware zero register: keep its value (an uninitialised `zero` would be undefined natively)
            zero_reg = template.strip() in ('$0', '$zero') and '=' not in seg
            edits.append((pin[0], pin[1], ' = 0' if zero_reg else ''))
            consumed.append(pin)
            report['pins'] += 1
            report['pins_by_file'][rel] += 1

    # --- 2. asm statements ----------------------------------------------------------------------------
    for a in ASM_TOKEN.finditer(masked):
        if any(cs <= a.start() < ce for cs, ce in consumed):
            continue
        parsed = parse_asm(text, masked, a.end())
        if not parsed:
            continue
        template, po, pe = parsed
        k = pe
        while masked[k:k + 1].isspace():
            k += 1
        if masked[k:k + 1] != ';':
            report['odd'].append((rel, text.count('\n', 0, a.start()) + 1, 'asm not followed by ;'))
            continue
        line = text.count('\n', 0, a.start()) + 1
        if template.strip() == '':
            assigns, output_only = asm_data_flow(text, masked, po, pe)
            if assigns:                                 # a tied output fed from a different expression: keep the data flow
                edits.append((a.start(), k + 1, ' '.join(assigns)))
                report['asm_assign'].append((rel, line, ' '.join(assigns)))
            else:
                edits.append((a.start(), k + 1, ''))
                report['barriers'] += 1
                report['barriers_by_file'][rel] += 1
                if output_only:
                    report['asm_output_only'].append((rel, line, text[a.start():k + 1].replace('\n', ' ')[:90]))
        else:
            report['real_asm'].append((rel, line, template.replace('\n', '\\n')[:70]))

    # apply edits back to front (drop overlapping ones defensively)
    edits.sort(key=lambda e: (e[0], e[1]))
    cleaned, last_end = [], -1
    for s, e, r in edits:
        if s >= last_end:
            cleaned.append((s, e, r))
            last_end = e
    out = text
    for s, e, r in reversed(cleaned):
        out = out[:s] + r + out[e:]

    # --- 3. volatile views (line based, .c files only) ----------------------------------------------------
    if rel.endswith('.c'):
        visible = report['headers'].closure_of(INCLUDE.findall(text))   # what this file's includes declare
        lines = out.split('\n')
        for i, l in enumerate(lines):
            m = VOLATILE_VIEW.match(l)
            if not m:
                continue
            decl = ' '.join(m.group('decl').split())
            nm = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*$', decl)
            if not nm:
                report['odd'].append((rel, i + 1, 'volatile view not understood: ' + l.strip()))
                continue
            name, dim = nm.group(1), nm.group(2)
            base = decl[:nm.start()].strip()
            if name not in visible or visible[name]:
                report['views_kept'] += 1     # no conflicting non-volatile header declaration: keep as is
                continue
            if dim is not None:
                if dim.strip('[]').strip().isdigit() or dim == '[]':
                    lines[i] = f'{m.group("ind")}#define {name} (({base}*){name})'
                    report['views'] += 1
                else:
                    report['odd'].append((rel, i + 1, 'volatile array view: ' + l.strip()))
                continue
            lines[i] = f'{m.group("ind")}#define {name} (*({base}*)&{name})'
            report['views'] += 1
        out = '\n'.join(lines)
    return out


NARROW_TYPES = {'s8', 'u8', 's16', 'u16'}
WIDE_TYPES = {'s32', 'u32', 'int'}
CAST_CALL = re.compile(r'\(\(\s*(?P<ret>[A-Za-z_][\w ]*?)\s*\(\s*\*\s*\)\s*\((?P<params>[^()]*(?:\([^()]*\)[^()]*)*)\)\s*\)\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*\(')
PROTOTYPE = re.compile(r'(?m)^[ \t]*(?:extern[ \t]+)?(?P<ret>[A-Za-z_][\w \t\*]*?)[ \t]*\b(?P<name>[A-Za-z_]\w*)[ \t]*\((?P<params>[^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)[ \t]*;')


def count_params(p):
    p = p.strip()
    if p in ('', 'void'):
        return 0
    if '...' in p:
        return -1
    depth, n = 0, 1
    for ch in p:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == ',' and depth == 0:
            n += 1
    return n


def narrow_return_calls(outdir, report):
    """Calls through a function-pointer cast of a function whose declared return type is NARROW (s8/u8/s16/u16), with the cast returning a
    32-bit integer: `((s32 (*)(void))world_input_get_tutorial_buttons)()`. The decomp writes them so (the plain call would make GCC 2.6.3 add an
    extension the target does not have); the retail callee has already sign/zero-extended the value into $v0 (an `lh`/`lbu` of its global, or an
    explicit sll/sra), and the caller uses the register as it is. A native callee returns the narrow value in al/ax with undefined upper bits, so the
    cast call reads garbage there (lockstep: world menu input 0x00008000 instead of 0xffff8000). Turning the call into a real call with a widening
    cast gives the C value of the narrow type -- exactly the extended register. Only cast calls whose argument list has as many parameters as the
    declaration are rewritten (the ones with stale arguments need a reviewed patch)."""
    decl = {}                                        # name -> (return type, number of parameters)
    for p in sorted((outdir / 'include').rglob('*.h')):
        for m in PROTOTYPE.finditer(mask(p.read_bytes().decode('utf-8'))):
            decl.setdefault(m.group('name'), (' '.join(m.group('ret').replace('extern', '').split()), count_params(m.group('params'))))
    defs = {p.stem: p for p in (outdir / 'src').rglob('*.c')}
    done = []
    for p in sorted(defs.values()):
        rel = p.relative_to(outdir).as_posix()
        if '/psyq/' in rel:
            continue
        text = p.read_bytes().decode('utf-8')
        masked = mask(text)
        edits = []
        for m in CAST_CALL.finditer(masked):
            ret, name = ' '.join(m.group('ret').split()), m.group('name')
            if ret not in WIDE_TYPES:
                continue
            info = decl.get(name)
            if name in defs:                            # the definition is authoritative (headers may omit static helpers)
                dm = re.search(r'(?m)^(?P<ret>[A-Za-z_][\w \t\*]*?)\b' + re.escape(name) + r'\s*\((?P<params>[^{;]*)\)\s*\{', mask(defs[name].read_bytes().decode('utf-8')))
                if dm:
                    info = (' '.join(dm.group('ret').split()), count_params(dm.group('params')))
            if not info or info[0] not in NARROW_TYPES or info[1] < 0 or count_params(m.group('params')) != info[1]:
                continue
            open_idx = m.end() - 1                      # the '(' that starts the argument list
            close = match_paren(masked, open_idx)
            if close < 0:
                continue
            edits.append((m.start(), close, '((%s)%s(%s))' % (ret, name, text[open_idx + 1:close - 1])))
            done.append((rel, text.count('\n', 0, m.start()) + 1, name, info[0]))
        if edits:
            for s0, e0, r0 in reversed(edits):
                text = text[:s0] + r0 + text[e0:]
            p.write_bytes(text.encode('utf-8'))
    report['narrow'] = done


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root')
    ap.add_argument('out')
    ap.add_argument('--report')
    args = ap.parse_args()
    root, outdir = Path(args.root), Path(args.out)
    report = dict(pins=0, barriers=0, views=0, views_kept=0, real_asm=[], odd=[], asm_assign=[], asm_output_only=[], asm_macros_dropped=[], pins_by_file=Counter(),
                  barriers_by_file=Counter(), files=0, headers=HeaderIndex(root), narrow=[])
    for sub in ('include', 'src'):
        for p in sorted((root / sub).rglob('*')):
            if not p.is_file() or p.suffix not in ('.c', '.h'):
                continue
            rel = p.relative_to(root).as_posix()
            text = p.read_bytes().decode('utf-8')
            new = transform(text, rel, report)
            dst = outdir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(new.encode('utf-8'))
            report['files'] += 1
    # 5. reviewed native patches (port/native/native_patches.py): retail-ABI accidents that need an explicit source-level fix
    patch_file = Path(__file__).resolve().parent.parent / 'native' / 'native_patches.py'
    npatched = 0
    if patch_file.exists():
        ns = {}
        exec(compile(patch_file.read_text(encoding='utf-8'), str(patch_file), 'exec'), ns)
        for pt in ns.get('PATCHES', []):
            for rel in (pt['files'] if 'files' in pt else [pt['file']]):          # `files`: the same edit in several twin sources
                dst = outdir / rel
                text = dst.read_bytes().decode('utf-8')
                want = pt.get('count', 1)
                if text.count(pt['old']) != want:
                    sys.exit(f"native patch must match {want} time(s) in {rel}: {pt['old']!r} (found {text.count(pt['old'])})")
                dst.write_bytes(text.replace(pt['old'], pt['new']).encode('utf-8'))
                npatched += 1
    narrow_return_calls(outdir, report)
    lines = [
        f"native patches applied:    {npatched} (port/native/native_patches.py)",
        f"narrow-return cast calls:  {len(report['narrow'])} made real calls (callee returns s8/u8/s16/u16, cast consumed it as 32 bits)",
        f"files written:             {report['files']}",
        f"register pins removed:     {report['pins']} (in {len(report['pins_by_file'])} files)",
        f"empty asm statements gone: {report['barriers']} (in {len(report['barriers_by_file'])} files)",
        f"volatile views -> macros:  {report['views']} (kept as declarations, no header conflict: {report['views_kept']})",
        f"asm turned into assignments: {len(report['asm_assign'])} (tied output fed from a different expression)",
        f"output-only asm deleted:   {len(report['asm_output_only'])} (output left undefined; see report)",
        f"asm #define blocks dropped: {len(report['asm_macros_dropped'])} (file-local macros with real asm; the shim provides them)",
        f"real asm statements left:  {len(report['real_asm'])} (in {len({r[0] for r in report['real_asm']})} files)",
        f"unrecognised constructs:   {len(report['odd'])}",
    ]
    print('\n'.join(lines))
    if args.report:
        with open(args.report, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n\n== real asm (needs a hand port) ==\n')
            for rel, line, t in report['real_asm']:
                f.write(f'{rel}:{line}: {t}\n')
            f.write('\n== asm turned into assignments ==\n')
            for rel, line, t in report['asm_assign']:
                f.write(f'{rel}:{line}: {t}\n')
            f.write('\n== output-only asm deleted (variable left undefined) ==\n')
            for rel, line, t in report['asm_output_only']:
                f.write(f'{rel}:{line}: {t}\n')
            f.write('\n== narrow-return cast calls rewritten as real calls ==\n')
            for rel, line, name, typ in report['narrow']:
                f.write(f'{rel}:{line}: {name} ({typ})\n')
            f.write('\n== unrecognised ==\n')
            for rel, line, t in report['odd']:
                f.write(f'{rel}:{line}: {t}\n')


if __name__ == '__main__':
    sys.exit(main())
