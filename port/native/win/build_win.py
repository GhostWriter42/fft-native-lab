"""build_win.py -- build the native game as a Windows program (no Docker), with the i686 MinGW-w64 GCC in port/build/toolchain.

The same build as the container's (port/native/boundary.sh + build_run_lockstep.sh), step for step:
  1. the generated tables (gen_symbols / gen_funcs / gen_modules / gen_fuzz / gen_hle, as lockstep.ps1 runs them);
  2. every game source of the portified tree (port/build/portable, port/tools/mktree.ps1) compiled natively; the divisions expanded with the MIPS
     semantics (port/tools/divfix.awk); every defined yaml function renamed to native_<name>, references to per-overlay data copies to <name>__<document>;
  3. the runtime pieces (software GTE, libgte, hand-written replacements, platform layer, R3000 interpreter, driver) and win/win_sys.c;
  4. the objects that define an SDK function the platform layer takes over, and the BIOS veneers, left out; the remaining yaml functions stubbed;
  5. linked with the symbol script (data and functions at their PS1 addresses) into port/build/win/fft_native.exe: a 32-bit, large-address-aware
     program (the PS1 RAM sits at 0x80000000).
Win32 C symbols carry a leading '_', so every symbol map is written with it.

  python port/native/win/build_win.py [--jobs 6] [--clean]
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PORT = Path(__file__).resolve().parents[2]
REPO = PORT.parent / 'fft_decomp'
NAT = PORT / 'native'
PT = PORT / 'build' / 'portable'
NB = PORT / 'build' / 'native' / 'ls'
OUT = PORT / 'build' / 'win'
TOOLS = PORT / 'build' / 'toolchain' / 'mingw32' / 'bin'
GCC, NM, OBJCOPY = str(TOOLS / 'gcc.exe'), str(TOOLS / 'nm.exe'), str(TOOLS / 'objcopy.exe')
MODS = ['main.yaml', 'battle.yaml', 'opening.yaml', 'wldcore.yaml', 'world.yaml', 'event.yaml', 'effect.yaml']
DIRS = ['src/main', 'src/battle', 'src/open', 'src/wldcore', 'src/world', 'src/event', 'src/effect', 'src/psyq/libgpu', 'src/psyq/libc', 'src/psyq/libapi',
        'src/psyq/libetc', 'src/psyq/libcd', 'src/psyq/libspu', 'src/psyq/libcard', 'src/psyq/libpress', 'src/psyq/suzuki']
BASE_CF = ['-m32', '-mno-ms-bitfields', '-mno-align-double', '-fpcc-struct-return', '-O1', '-w', '-std=gnu89',          # (the two: Linux i386 struct layout, not MinGW's MS one)
           '-funsigned-char', '-fcommon', '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-stack-protector',
           '-fno-strict-aliasing', '-fno-aggressive-loop-optimizations', '-fwrapv', '-fno-delete-null-pointer-checks', '-fno-asynchronous-unwind-tables', '-nostdinc',
           f'-I{NAT / "shim"}', f'-I{NAT / "gte"}', f'-I{PT / "include"}', '-include', 'psx/gte_inline.h']
GAME_FLAGS = ['-ftrivial-auto-var-init=zero', '-fno-omit-frame-pointer', '-finstrument-functions']
RT_CF = BASE_CF[:-2] + ['-fno-tree-loop-distribute-patterns', '-fno-omit-frame-pointer', f'-I{NAT}', f'-I{NB}', f'-I{PORT / "build" / "native"}',
                        '-include', 'psx/gte_inline.h', '-DPC_SCHEME', '-DSCENARIO_TITLE', '-DLOCKSTEP_THREAD_WINDOW', '-DMAX_FRAMES=60', '-DLOG_LIMIT=0'] + GAME_FLAGS + [
          # (as lockstep.ps1's EXTRA_CFLAGS: the thread stacks in the mapped window, the same instrumentation as the container build)
          '-finstrument-functions-exclude-file-list=native/rt.c,native/hle/hle.c,native/hle/gpu.c,native/hle/card.c,native/r3000/r3000.c,native/gte/gte.c,native/lockstep.c,native/bios_rt.c,sym_table.c,modules.c']


def find_awk():
    for c in (shutil.which('awk'), r'C:\Program Files\Git\usr\bin\awk.exe', r'C:\Program Files\Git\usr\bin\gawk.exe'):
        if c and Path(c).exists():
            return c
    sys.exit('awk (Git for Windows) is needed for the division expansion')


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)
    return r.returncode, r.stdout, r.stderr


def nm_defined_globals(obj):
    _, out, _ = run([NM, '--defined-only', '-g', str(obj)])
    return [l.split()[2] for l in out.splitlines() if len(l.split()) >= 3]


def nm_undefined(obj):
    _, out, _ = run([NM, '-u', str(obj)])
    return [l.split()[-1] for l in out.splitlines() if l.strip()]


def rename_defs(obj, fn_names):
    m = [f'{s} _native_{s[1:]}' for s in nm_defined_globals(obj) if s.startswith('_') and s[1:] in fn_names]
    if m:
        mp = Path(str(obj) + '.map')
        mp.write_text('\n'.join(m) + '\n')
        run([OBJCOPY, f'--redefine-syms={mp}', str(obj)])
        mp.unlink()


def compile_one(src, obj, cflags, awk, divfix, cwd):
    if divfix:
        s = Path(str(obj) + '.s')
        rc, _, err = run([GCC] + cflags + ['-S', '-o', str(s), Path(src).as_posix()], cwd=cwd)
        if rc == 0:
            fixed = Path(str(obj) + '.f.s')
            with open(s, 'rb') as fi, open(fixed, 'wb') as fo:
                rc = subprocess.run([awk, '-f', str(PORT / 'tools' / 'divfix.awk')], stdin=fi, stdout=fo).returncode
            if rc == 0:
                rc, _, err = run([GCC, '-m32', '-c', '-x', 'assembler', '-o', str(obj), str(fixed)])
            fixed.unlink(missing_ok=True)
        s.unlink(missing_ok=True)
        return rc, err
    rc, _, err = run([GCC] + cflags + ['-c', '-o', str(obj), Path(src).as_posix()], cwd=cwd)     # (forward slashes: -finstrument-functions-exclude-file-list matches them)
    return rc, err


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--clean', action='store_true')
    ap.add_argument('--no-divfix', action='store_true')
    ap.add_argument('--watch', action='append', default=[], help='ADDR (hex): a RAM word the replay mode watches (as lockstep.ps1 -Watch)')
    ap.add_argument('--define', action='append', default=[], help='extra -D for the runtime pieces (e.g. REPLAY_FRAME=875: the function-level replay of one frame)')
    a = ap.parse_args()
    if not Path(GCC).exists():
        sys.exit(f'no compiler at {GCC} (unpack the WinLibs i686 GCC 12 zip into port/build/toolchain)')
    if not (PT / '.stamp').exists():
        sys.exit('no portified tree: run port/tools/mktree.ps1 first')
    awk = find_awk()
    divfix = not a.no_divfix
    if a.clean and OUT.exists():
        shutil.rmtree(OUT)
    (OUT / 'o').mkdir(parents=True, exist_ok=True)
    (OUT / 'x').mkdir(parents=True, exist_ok=True)

    # 1. the generated tables (host Python, the same as lockstep.ps1)
    NB.mkdir(parents=True, exist_ok=True)
    for args in (['gen_symbols.py', str(REPO), str(NB / 'symbols_pc.ld'), '--functions', '--native-prefix=native_'],
                 ['gen_funcs.py', str(REPO), str(NB / 'func_addrs.c')],
                 ['gen_modules.py', str(REPO), str(NB / 'modules.c'), '--native-prefix=native_'],
                 ['gen_fuzz.py', str(REPO), str(NB), '--native-prefix=native_'],
                 ['gen_hle.py', str(REPO), str(NB / 'hle_generated.c')]):
        r = subprocess.run([sys.executable, str(NAT / args[0])] + args[1:] + MODS, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'{args[0]} failed:\n{r.stdout}{r.stderr}')
    watch_h = ''.join('{ 0x%08xu, "%s" },\n' % (int(w, 16), w) for w in a.watch) or '\n'
    for name, text in (('dump_around.h', ''), ('pad_script.h', '{ 0u, 0u }\n'), ('watch.h', watch_h), ('peek.h', '\n')):
        (NB / name).write_text(text)
    fn_names = set((NB / 'fn_names.txt').read_text().split())
    scoped = set((NB / 'scoped_syms.txt').read_text().split()) if (NB / 'scoped_syms.txt').exists() else set()
    fn_docs = dict(l.split()[:2] for l in (NB / 'fn_docs.txt').read_text().splitlines() if len(l.split()) >= 2) if (NB / 'fn_docs.txt').exists() else {}

    # 2. the game sources
    replaced = set((NAT / 'replacements' / 'replaced.txt').read_text().split()) if (NAT / 'replacements' / 'replaced.txt').exists() else set()
    srcs = [p for d in DIRS for p in sorted((PT / d).rglob('*.c'))]
    srcs = [p for p in srcs if p.relative_to(PT).as_posix() not in replaced]
    # cache: an object is reused while its source's CONTENT is unchanged (mktree rewrites every file, so times say nothing) and neither the flags
    # nor any header nor the symbol maps changed (those invalidate everything)
    import json
    hdr = hashlib.md5()
    for h in sorted((PT / 'include').rglob('*.h')) + sorted((NAT / 'shim').rglob('*.h')) + sorted((NAT / 'gte').rglob('*.h')):
        hdr.update(h.read_bytes())
    for f in ('fn_names.txt', 'scoped_syms.txt', 'fn_docs.txt'):
        if (NB / f).exists():
            hdr.update((NB / f).read_bytes())
    key = hashlib.md5((' '.join(BASE_CF + GAME_FLAGS) + f' divfix={divfix} ' + hdr.hexdigest()).encode()).hexdigest()
    keyfile = OUT / 'o' / 'build_key.txt'
    srcmap_path = OUT / 'o' / 'sources.json'
    if not keyfile.exists() or keyfile.read_text() != key:
        for o in (OUT / 'o').glob('*.o'):
            o.unlink()
        srcmap = {}
    else:
        srcmap = json.loads(srcmap_path.read_text()) if srcmap_path.exists() else {}
    game_cf = BASE_CF + GAME_FLAGS

    def build_game(src):
        rel = src.relative_to(PT).as_posix()
        obj = OUT / 'o' / (rel.replace('/', '_') + '.o')
        digest = hashlib.md5(src.read_bytes()).hexdigest()
        if obj.exists() and srcmap.get(rel) == digest:
            return rel, True, ''
        srcmap.pop(rel, None)
        rc, err = compile_one(src, obj, game_cf, awk, divfix, str(PT))
        if rc:
            obj.unlink(missing_ok=True)
            return rel, False, err
        rename_defs(obj, fn_names)
        doc = fn_docs.get(src.stem)
        if doc and scoped:
            m = [f'{s} {s}__{doc}' for s in nm_undefined(obj) if s.startswith('_') and s[1:] in scoped]
            if m:
                mp = Path(str(obj) + '.smap')
                mp.write_text('\n'.join(m) + '\n')
                run([OBJCOPY, f'--redefine-syms={mp}', str(obj)])
                mp.unlink()
        srcmap[rel] = digest
        return rel, True, ''

    print(f'game sources: {len(srcs)} (compiling with {a.jobs} jobs)', flush=True)
    failed = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for rel, ok, err in ex.map(build_game, srcs):
            if not ok:
                failed.append((rel, err))
    keyfile.write_text(key)
    srcmap_path.write_text(json.dumps(srcmap))
    print(f'compiled: {len(srcs) - len(failed)}, failed: {len(failed)}')
    for rel, err in failed[:20]:
        print('  ', rel, (err.strip().splitlines() or [''])[0][:150])

    # 3. the runtime pieces
    p_gte = PT / 'src' / 'psyq' / 'libgte'
    renamed_rt = [NAT / 'gte' / 'libgte_native.c', p_gte / 'rsin.c', p_gte / 'sin_1.c', p_gte / 'rcos.c', p_gte / 'ratan2.c', p_gte / 'csqrt.c',
                  p_gte / 'psyq_gte_csqrt_kernel.c', NAT / 'replacements' / 'main_asm.c', NAT / 'replacements' / 'battle_asm.c', NAT / 'replacements' / 'battle_asm2.c',
                  NAT / 'replacements' / 'battle_asm3.c', NAT / 'replacements' / 'battle_thread.c', NAT / 'replacements' / 'world_asm.c', NAT / 'replacements' / 'world_thread.c']
    plain_rt = [NAT / 'gte' / 'gte.c', NAT / 'r3000' / 'r3000.c', NAT / 'hle' / 'hle.c', NAT / 'hle' / 'gpu.c', NAT / 'hle' / 'spu.c', NAT / 'hle' / 'card.c',
                NB / 'hle_generated.c', NB / 'modules.c', NB / 'sym_table.c', NAT / 'bios_rt.c', NAT / 'lockstep.c', NAT / 'rt.c']
    rt_cf = RT_CF + (['-DDECOMP_HAS_GS_SORTPOLY'] if (PT / 'src' / 'world' / 'world_gs_sortpoly.c').exists() else []) + ['-D' + d for d in a.define]
    for o in (OUT / 'x').glob('*.o'):
        o.unlink()

    def build_rt(item):
        src, rename = item
        obj = OUT / 'x' / f'x_{src.stem}.o'
        rc, err = compile_one(src, obj, rt_cf, awk, divfix and rename, str(PORT))
        if rc:
            return src.name, err
        if rename:
            rename_defs(obj, fn_names)
        return src.name, ''

    with ThreadPoolExecutor(a.jobs) as ex:
        rt_failed = [(n, e) for n, e in ex.map(build_rt, [(s, True) for s in renamed_rt] + [(s, False) for s in plain_rt]) if e]
    rc, _, err = run([GCC, '-m32', '-O1', '-w', '-c', '-o', str(OUT / 'x' / 'x_win_sys.o'), str(NAT / 'win' / 'win_sys.c')])
    if rc:
        rt_failed.append(('win_sys.c', err))
    if rt_failed:
        for n, e in rt_failed:
            print(f'runtime piece {n} failed:\n{e[:3000]}')
        sys.exit(1)

    # 4. which game objects are linked
    hle = set(l.strip()[len('native_'):] for l in (NB / 'hle_natives.txt').read_text().splitlines() if l.strip())
    veneer_srcs = set()
    for p in (PT / 'src' / 'psyq').rglob('*.c'):
        if 'PSYQ_BIOS_' in p.read_text(errors='replace'):
            veneer_srcs.add(p.relative_to(PT).as_posix().replace('/', '_') + '.o')
    objs, dropped = [], 0
    defined = set()
    undef = set()
    for o in sorted((OUT / 'o').glob('*.o')):
        d = nm_defined_globals(o)
        if o.name in veneer_srcs or any(s.startswith('_native_') and s[len('_native_'):] in hle for s in d):
            dropped += 1
            continue
        objs.append(o)
        defined.update(d)
    for o in (OUT / 'x').glob('*.o'):
        defined.update(nm_defined_globals(o))
    print(f'game objects: {len(objs)} linked, {dropped} dropped (BIOS veneers and SDK functions the platform layer takes over)')
    want = {'_native_' + n for n in fn_names}
    missing = sorted(want - defined)
    lines = [f'unsigned g_stub_calls; unsigned g_stub_hits[{len(missing) + 1}];']
    for i, u in enumerate(missing):
        lines.append(f'int {u[1:]}() {{ g_stub_calls++; g_stub_hits[{i}]++; return 0; }}')
    lines.append('const char* const g_stub_names[] = {')
    lines += [f'  "{u[len("_native_"):]}",' for u in missing]
    lines += ['  0 };', f'const int g_stub_count = {len(missing)};']
    (OUT / 'stubs.c').write_text('\n'.join(lines) + '\n')
    rc, _, err = run([GCC] + rt_cf + ['-c', '-o', str(OUT / 'x' / 'x_stubs.o'), str(OUT / 'stubs.c')])
    if rc:
        sys.exit('stubs: ' + err)
    print(f'stubbed yaml functions with no native definition: {len(missing)}')

    # 5. link: the symbol script with Win32's leading underscore
    ld = OUT / 'symbols_win.ld'
    out = []
    for line in (NB / 'symbols_pc.ld').read_text().splitlines():
        m = re.match(r'\s*([A-Za-z_]\w*)\s*=\s*(0x[0-9a-fA-F]+|[A-Za-z_]\w*)\s*;', line)
        if m:
            v = m.group(2)
            out.append(f'_{m.group(1)} = {v if v.startswith("0x") else "_" + v};')        # an address, or an alias of another symbol
    out.append('__end = __bss_end__;')
    ld.write_text('\n'.join(out) + '\n')
    rsp = OUT / 'link.rsp'
    rsp.write_text('\n'.join('"' + str(p).replace('\\', '/') + '"' for p in objs + sorted((OUT / 'x').glob('*.o'))) + '\n')
    exe = OUT / 'fft_native.exe'
    rc, so, err = run([GCC, '-m32', '-nostdlib', '-nostartfiles', '-Wl,--large-address-aware', '-Wl,--disable-dynamicbase', '-Wl,-e,__start',
                       '-Wl,--stack,0x400000', '-o', str(exe), f'@{rsp}', str(ld), '-lkernel32', '-lgcc'])
    if rc:
        errs = [l for l in err.splitlines() if 'undefined reference' in l]
        names = sorted(set(re.findall(r"undefined reference to `([^']+)'", err)))
        print(f'link FAILED: {len(errs)} undefined references, {len(names)} names: {" ".join(names[:60])}')
        print('\n'.join(l for l in err.splitlines() if 'undefined reference' not in l)[:3000])
        sys.exit(1)
    run([NM, '-n', str(exe)], cwd=str(OUT))
    with open(OUT / 'fft_native.nm', 'w') as f:
        subprocess.run([NM, '-n', str(exe)], stdout=f)
    print(f'linked {exe} ({exe.stat().st_size >> 10} KiB)')


if __name__ == '__main__':
    main()
