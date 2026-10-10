"""return_type_scan.py -- GCC's own check for functions that can reach their end without a return value, over every game source of the
portified tree (port/build/portable), with the Windows toolchain (objects are discarded).

Such a function returns whatever the CPU's return register holds: on the PS1 a deterministic leftover in $v0, natively whatever is in eax. Every hit
whose value a caller uses needs a reviewed patch (port/native/native_patches.py) that returns the retail value (port/tools/retail_dis.py shows it).

  python port/tools/return_type_scan.py [--jobs 6]
"""
import argparse
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
PT = PORT / 'build' / 'portable'
NAT = PORT / 'native'
GCC = str(PORT / 'build' / 'toolchain' / 'mingw32' / 'bin' / 'gcc.exe')
DIRS = ['src/main', 'src/battle', 'src/open', 'src/wldcore', 'src/world', 'src/event', 'src/effect']
CF = ['-m32', '-O1', '-std=gnu89', '-funsigned-char', '-fcommon', '-ffreestanding', '-fno-builtin', '-nostdinc', '-Wreturn-type',
      f'-I{NAT / "shim"}', f'-I{NAT / "gte"}', f'-I{PT / "include"}', '-include', 'psx/gte_inline.h', '-c', '-o', 'NUL']      # (a real compile: the falls-off-the-end check runs in the optimiser)


def scan(src):
    r = subprocess.run([GCC] + CF + [src.as_posix()], capture_output=True, text=True, errors='replace', cwd=str(PT))
    return [(src.relative_to(PT).as_posix(), l) for l in r.stderr.splitlines() if 'control reaches end of non-void' in l or 'with no value, in function returning non-void' in l]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=6)
    a = ap.parse_args()
    srcs = [p for d in DIRS for p in sorted((PT / d).rglob('*.c'))]
    hits = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for h in ex.map(scan, srcs):
            hits += h
    for rel, line in sorted(hits):
        m = re.search(r":(\d+):\d+: warning: (.*?) \[", line)
        print(f'{rel}:{m.group(1) if m else "?"}  {m.group(2) if m else line}')
    print(f'{len(hits)} functions can return without a value ({len(srcs)} sources checked)')


if __name__ == '__main__':
    main()
