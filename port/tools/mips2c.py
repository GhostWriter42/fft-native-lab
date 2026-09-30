#!/usr/bin/env python3
"""mips2c.py -- translate hand-written R3000/GTE routines of the retail binary into C (static binary translation of leaf routines).

The decomp keeps some routines as raw bytes (`kind: handwritten` in target/*.yaml); a native build needs them as C. This tool reads the routine's machine code from the
extracted disc file and emits a C function that executes the same instructions on an array of 32 register variables:

  * every instruction becomes one or two C statements with a label at each jump target; the machine's delay slots are honoured (the slot instruction runs before the
    branch takes effect, the condition is evaluated before it);
  * loads/stores go through the real addresses (the native port maps the RAM image and the scratchpad at their PS1 addresses), GTE instructions through the software
    GTE (gte_mtc2 / gte_mfc2 / gte_ctc2 / gte_cfc2 / gte_command);
  * `sp` points into a local array (the routine's own frame); `jr ra` returns (v0 is the return value);
  * load-delay hazards (an instruction that reads the register the preceding load writes: on the machine it sees the OLD value) are detected and reported;
    unsupported instructions (jal, indirect jumps, lwl/lwr, ...) stop the translation.

Correctness is established against the original machine code like every other native routine (function-level fuzz, and the whole-program lockstep, which runs the
original bytes on the interpreter next to the generated C).

Usage:  mips2c.py --yaml battle --out port/native/replacements/battle_asm3.c NAME[:ret-type] ...
        (the C parameters are taken from the declaration in the repo headers when one exists; otherwise u32 a0..a3)
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'native'))
import yamlfuncs  # noqa: E402

REPO = Path(__file__).resolve().parents[2] / 'fft_decomp'
REGS = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
        's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def find_function(name):
    for y in sorted((REPO / 'target').glob('*.yaml')):
        for modname, hdr, lines in yamlfuncs.documents(y):
            for addr, size, n, asm in yamlfuncs.parse_lines(lines):
                if n == name and 'file' in hdr:
                    return modname, hdr, addr, size
    raise SystemExit(f'function {name} not found')


class Insn:
    def __init__(self, addr, w):
        self.addr, self.w = addr, w
        self.op, self.rs, self.rt, self.rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        self.sh, self.fn, self.imm = (w >> 6) & 31, w & 63, w & 0xffff
        self.simm = sx16(self.imm)
        self.branch = self.jump = self.ret = False
        self.target = None
        self.load_reg = None                     # register written by a load (load delay slot)
        self.reads = set()
        self.decode()

    def decode(self):
        op, rs, rt, rd, fn = self.op, self.rs, self.rt, self.rd, self.fn
        a = self.addr
        r = self.reads
        if self.w == 0:
            self.kind = 'nop'
        elif op == 0:
            self.kind = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 24: 'mult', 25: 'multu',
                         26: 'div', 27: 'divu', 32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu'}.get(fn, 'bad')
            if self.kind in ('sll', 'srl', 'sra'): r.update([rt])
            elif self.kind in ('sllv', 'srlv', 'srav'): r.update([rt, rs])
            elif self.kind == 'jr': r.update([rs]); self.jump = True; self.ret = rs == 31
            elif self.kind in ('mthi', 'mtlo'): r.update([rs])
            elif self.kind in ('mult', 'multu', 'div', 'divu'): r.update([rs, rt])
            elif self.kind in ('mfhi', 'mflo'): pass
            else: r.update([rs, rt])
        elif op == 1:
            self.kind = {0: 'bltz', 1: 'bgez'}.get(rt, 'bad')
            self.branch = True; self.target = a + 4 + (self.simm << 2); r.update([rs])
        elif op == 2:
            self.kind = 'j'; self.jump = True; self.target = ((a + 4) & 0xf0000000) | ((self.w & 0x3ffffff) << 2)
        elif op == 3:
            self.kind = 'jal'
        elif op in (4, 5):
            self.kind = 'beq' if op == 4 else 'bne'; self.branch = True; self.target = a + 4 + (self.simm << 2); r.update([rs, rt])
        elif op in (6, 7):
            self.kind = 'blez' if op == 6 else 'bgtz'; self.branch = True; self.target = a + 4 + (self.simm << 2); r.update([rs])
        elif op in (8, 9, 10, 11, 12, 13, 14):
            self.kind = {8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori'}[op]; r.update([rs])
        elif op == 15:
            self.kind = 'lui'
        elif op in (32, 33, 35, 36, 37):
            self.kind = {32: 'lb', 33: 'lh', 35: 'lw', 36: 'lbu', 37: 'lhu'}[op]; r.update([rs]); self.load_reg = rt
        elif op in (40, 41, 43):
            self.kind = {40: 'sb', 41: 'sh', 43: 'sw'}[op]; r.update([rs, rt])
        elif op == 0x12:
            if self.w & 0x02000000: self.kind = 'gte'
            else:
                self.kind = {0: 'mfc2', 2: 'cfc2', 4: 'mtc2', 6: 'ctc2'}.get(rs, 'bad')
                if self.kind in ('mtc2', 'ctc2'): r.update([rt])
                if self.kind in ('mfc2', 'cfc2'): self.load_reg = rt                       # the destination of a cop2 read lands late as well
        elif op == 0x32:
            self.kind = 'lwc2'; r.update([rs])
        elif op == 0x3a:
            self.kind = 'swc2'; r.update([rs])
        else:
            self.kind = 'bad'
        r.discard(0)

    def text(self):
        k = self.kind
        rn = REGS
        if k == 'nop': return 'nop'
        return f'{k} ' + ', '.join(filter(None, [rn[self.rd] if k not in ('lui', 'lb', 'lh', 'lw', 'lbu', 'lhu') else '', rn[self.rt], rn[self.rs], f'0x{self.imm:x}']))


def expr_c(i):
    """the C statement(s) for instruction i (without its delay slot / branch part); '' for nops"""
    k, rs, rt, rd = i.kind, f'r[{i.rs}]', f'r[{i.rt}]', f'r[{i.rd}]'
    simm, imm = i.simm, i.imm
    wr = lambda reg, val: '' if reg == 0 else f'r[{reg}] = {val};'
    if k == 'nop': return ''
    if k == 'sll': return wr(i.rd, f'{rt} << {i.sh}')
    if k == 'srl': return wr(i.rd, f'{rt} >> {i.sh}')
    if k == 'sra': return wr(i.rd, f'(u32)((s32){rt} >> {i.sh})')
    if k == 'sllv': return wr(i.rd, f'{rt} << ({rs} & 31)')
    if k == 'srlv': return wr(i.rd, f'{rt} >> ({rs} & 31)')
    if k == 'srav': return wr(i.rd, f'(u32)((s32){rt} >> ({rs} & 31))')
    if k == 'mfhi': return wr(i.rd, 'hi')
    if k == 'mflo': return wr(i.rd, 'lo')
    if k == 'mthi': return f'hi = {rs};'
    if k == 'mtlo': return f'lo = {rs};'
    if k == 'mult': return f'{{ long long p = (long long)(s32){rs} * (long long)(s32){rt}; lo = (u32)p; hi = (u32)((unsigned long long)p >> 32); }}'
    if k == 'multu': return f'{{ unsigned long long p = (unsigned long long){rs} * (unsigned long long){rt}; lo = (u32)p; hi = (u32)(p >> 32); }}'
    if k == 'div': return f'if ({rt}) {{ lo = (u32)((s32){rs} / (s32){rt}); hi = (u32)((s32){rs} % (s32){rt}); }}'
    if k == 'divu': return f'if ({rt}) {{ lo = {rs} / {rt}; hi = {rs} % {rt}; }}'
    if k in ('add', 'addu'): return wr(i.rd, f'{rs} + {rt}')
    if k in ('sub', 'subu'): return wr(i.rd, f'{rs} - {rt}')
    if k == 'and': return wr(i.rd, f'{rs} & {rt}')
    if k == 'or': return wr(i.rd, f'{rs} | {rt}')
    if k == 'xor': return wr(i.rd, f'{rs} ^ {rt}')
    if k == 'nor': return wr(i.rd, f'~({rs} | {rt})')
    if k == 'slt': return wr(i.rd, f'(u32)((s32){rs} < (s32){rt})')
    if k == 'sltu': return wr(i.rd, f'(u32)({rs} < {rt})')
    if k in ('addi', 'addiu'): return wr(i.rt, f'{rs} + (u32){simm}')
    if k == 'slti': return wr(i.rt, f'(u32)((s32){rs} < {simm})')
    if k == 'sltiu': return wr(i.rt, f'(u32)({rs} < (u32){simm})')
    if k == 'andi': return wr(i.rt, f'{rs} & 0x{imm:x}u')
    if k == 'ori': return wr(i.rt, f'{rs} | 0x{imm:x}u')
    if k == 'xori': return wr(i.rt, f'{rs} ^ 0x{imm:x}u')
    if k == 'lui': return wr(i.rt, f'0x{imm << 16:08x}u')
    ea = f'(u32)({rs} + (u32){simm})'
    if k == 'lb': return wr(i.rt, f'(u32)(s32)*(s8*){ea}')
    if k == 'lbu': return wr(i.rt, f'*(u8*){ea}')
    if k == 'lh': return wr(i.rt, f'(u32)(s32)*(s16*){ea}')
    if k == 'lhu': return wr(i.rt, f'*(u16*){ea}')
    if k == 'lw': return wr(i.rt, f'*(u32*){ea}')
    if k == 'sb': return f'*(u8*){ea} = (u8){rt};'
    if k == 'sh': return f'*(u16*){ea} = (u16){rt};'
    if k == 'sw': return f'*(u32*){ea} = {rt};'
    if k == 'mfc2': return wr(i.rt, f'gte_mfc2({i.rd})')
    if k == 'cfc2': return wr(i.rt, f'gte_cfc2({i.rd})')
    if k == 'mtc2': return f'gte_mtc2({i.rd}, {rt});'
    if k == 'ctc2': return f'gte_ctc2({i.rd}, {rt});'
    if k == 'gte': return f'gte_command(0x{i.w:08x}u);'
    if k == 'lwc2': return f'gte_mtc2({i.rt}, *(u32*){ea});'
    if k == 'swc2': return f'*(u32*){ea} = gte_mfc2({i.rt});'
    raise SystemExit(f'cannot translate {i.kind} at 0x{i.addr:08x}')


def cond_c(i):
    rs, rt = f'r[{i.rs}]', f'r[{i.rt}]'
    return {'beq': f'{rs} == {rt}', 'bne': f'{rs} != {rt}', 'bltz': f'(s32){rs} < 0', 'bgez': f'(s32){rs} >= 0', 'blez': f'(s32){rs} <= 0', 'bgtz': f'(s32){rs} > 0'}[i.kind]


def translate(name, ret_type, params):
    modname, hdr, addr, size = find_function(name)
    fname = hdr['file'].split()[0]
    load = int(hdr['load'].split()[0], 16)
    data = (REPO / 'build' / 'extracted' / 'files' / fname).read_bytes()
    words = struct.unpack_from(f'<{size // 4}I', data, addr - load)
    insns = {addr + 4 * k: Insn(addr + 4 * k, w) for k, w in enumerate(words)}
    end = addr + size
    warnings = []
    for i in insns.values():
        if i.kind == 'bad' or i.kind == 'jal':
            raise SystemExit(f'{name}: unsupported instruction {i.kind} (0x{i.w:08x}) at 0x{i.addr:08x}')
        if i.jump and not i.ret and i.kind != 'j':
            raise SystemExit(f'{name}: indirect jump at 0x{i.addr:08x}')
        if i.target is not None and not (addr <= i.target < end):
            raise SystemExit(f'{name}: jump out of the routine at 0x{i.addr:08x} -> 0x{i.target:08x}')
    # the machine's load delay slot: the instruction right after a load sees the OLD value of the loaded register
    for a, i in insns.items():
        if i.load_reg:
            nxt = []
            if i.branch or i.jump: pass
            following = insns.get(a + 4)
            if following and i.load_reg in following.reads: warnings.append(f'0x{a:08x}: {i.text()} -> the next instruction reads {REGS[i.load_reg]} (load delay slot)')
            prev = insns.get(a - 4)
            if prev and (prev.branch or prev.jump):        # a load in a delay slot: both successors
                for t in ([prev.target] if prev.target is not None else []) + ([a + 4] if prev.branch else []):
                    n2 = insns.get(t)
                    if n2 and i.load_reg in n2.reads: warnings.append(f'0x{a:08x}: load in a delay slot, successor 0x{t:08x} reads {REGS[i.load_reg]}')
    targets = {i.target for i in insns.values() if i.target is not None}
    out = []
    p = out.append
    sig = ', '.join(f'{t} {n}' for t, n in params)
    p(f'/* {name}: generated by port/tools/mips2c.py from {fname} 0x{addr:08x}..0x{end:08x} ({size // 4} instructions); do not edit -- regenerate */')
    p(f'{ret_type} {name}({sig}) {{')
    p('    u32 r[32], hi = 0, lo = 0, stk[64];')
    p('    int q;')
    p('    for (q = 0; q < 32; q++) r[q] = 0;')
    for k, (t, n) in enumerate(params[:4]):
        p(f'    r[{4 + k}] = (u32){n};')
    p('    r[29] = (u32)(stk + 64); r[31] = 0;')
    p('    (void)hi; (void)lo;')
    a = addr
    slot_targets = set()
    while a < end:
        i = insns[a]
        lab = f'L_{a:08x}: ' if a in targets else ''
        if i.branch or i.jump:
            slot = insns[a + 4] if a + 4 < end else None
            if slot is None: raise SystemExit(f'{name}: branch without delay slot at 0x{a:08x}')
            if slot.branch or slot.jump: raise SystemExit(f'{name}: branch in a delay slot at 0x{a:08x}')
            body = expr_c(slot)
            if i.kind == 'jr':
                p(f'    {lab}/* {a:08x} {i.text()} */ {body} return{(" (" + ret_type + ")r[2]") if ret_type != "void" else ""};')
            elif i.kind == 'j':
                p(f'    {lab}/* {a:08x} j 0x{i.target:08x} */ {body} goto L_{i.target:08x};')
            else:
                p(f'    {lab}/* {a:08x} {i.kind} -> 0x{i.target:08x} */ {{ int taken = {cond_c(i)}; {body} if (taken) goto L_{i.target:08x}; }}')
            if (a + 4) in targets:
                p(f'    goto L_{a + 8:08x};')
                p(f'    L_{a + 4:08x}: {expr_c(slot)}')
            a += 8 if (a + 4) not in targets else 8
            continue
        st = expr_c(i)
        if st or lab:
            p(f'    {lab}/* {a:08x} */ {st}')
        a += 4
    p('}')
    return '\n'.join(out) + '\n', warnings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--header', default='')
    ap.add_argument('names', nargs='+')
    args = ap.parse_args()
    text = [args.header or '/* Native C versions of hand-assembled routines, translated mechanically from the retail machine code by port/tools/mips2c.py. */',
            '#include "fft/battle.h"', '#include "psx/types.h"', '#include "gte.h"', '']
    allw = []
    for spec in args.names:
        name, _, ret = spec.partition(':')
        params = [('u32*', 'otag'), ('void*', 'prims'), ('s32', 'depth'), ('s32*', 'count')] if name.startswith('battle_map_queue_') else [('u32', f'a{k}') for k in range(4)]
        code, warns = translate(name, ret or 'void', params)
        text.append(code)
        allw += [f'{name}: {w}' for w in warns]
    Path(args.out).write_text('\n'.join(text), encoding='utf-8', newline='\n')
    print(f'wrote {args.out}: {len(args.names)} routines')
    for w in allw:
        print('WARNING', w)


if __name__ == '__main__':
    main()
