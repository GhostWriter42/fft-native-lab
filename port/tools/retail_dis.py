"""retail_dis.py -- disassemble a function of the original game (the bytes from your extracted disc files) by name, with symbol names.

  port/build/venv/Scripts/python.exe port/tools/retail_dis.py battle_action_can_unit_react_1 [more names ...]

Needs capstone (pip install capstone into port/build/venv). Used to read what a retail function leaves in $v0 where the decomp's C has no
return value (native_patches.py).
"""
import re
import sys
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / 'fft_decomp' / 'target'
FILES = ROOT / 'fft_decomp' / 'build' / 'extracted' / 'files'


def modules():
    """[(yaml document text, file, load address)]"""
    out = []
    for y in sorted(TARGET.glob('*.yaml')):
        for doc in y.read_text().split('\n---'):
            f = re.search(r'^file: (\S+)', doc, re.M)
            load = re.search(r'^load: (0x[0-9a-fA-F]+)', doc, re.M)
            if f and load:
                out.append((doc, f.group(1), int(load.group(1), 16)))
            elif y.stem == 'main':
                out.append((doc, 'SCUS_942.21', 0x8000f800))
    return out


def symbols():
    names = {}
    for y in TARGET.glob('*.yaml'):
        for a, n in re.findall(r'addr: (0x[0-9a-fA-F]+)(?:, size: \d+)?, name: (\w+)', y.read_text()):
            names.setdefault(int(a, 16), n)
    return names


def main():
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
    syms = symbols()
    mods = modules()
    for name in sys.argv[1:]:
        for doc, fname, load in mods:
            m = re.search(r'addr: (0x[0-9a-fA-F]+), size: (\d+), name: ' + re.escape(name) + r'\b', doc)
            if not m:
                continue
            addr, size = int(m.group(1), 16), int(m.group(2))
            data = (FILES / fname).read_bytes()
            off = addr - load + (0x800 if fname == 'SCUS_942.21' else 0)     # the main executable has a 2 KiB PS-X EXE header
            print(f'== {name} ({fname}, 0x{addr:08x}, {size} bytes)')
            for ins in md.disasm(data[off:off + size], addr):
                ops = ins.op_str
                for t in re.findall(r'0x[0-9a-f]{8}', ops):
                    v = int(t, 16)
                    if v in syms:
                        ops = ops.replace(t, f'{t} <{syms[v]}>')
                print(f'  {ins.address:08x}: {ins.mnemonic:8s} {ops}')
            break
        else:
            print(f'== {name}: not found in the yaml')


if __name__ == '__main__':
    main()
