"""make_test_states.py -- (re)make the owner's test save states for the GPU viewer's F5..F7 (slots 1..3), with the Windows build.

A machine state loads only into the program build that wrote it, so after every rebuild of port/build/win/fft_native.exe run this again.

  slot 1  the first battle (Orbonne) just started: frame 13000 of the owner's 2026-10-03 session (its input log), Ramza on manual
  slot 2  the end of that battle: from slot 1, Ramza switched to auto-battle (a memory poke) and Circle tapped once a second (spell quotes and
          messages wait for a button) until just after the last enemy falls (frame 33500); the victory scene starts by itself after loading
  slot 3  the start of the second battle (Gariland, the unit deployment screen, frame 48000): from slot 2, Circle through the scenes, Cross
          to leave the save screen without saving

The states are made with sound on (the sound chip's memory is part of a state) on a throw-away memory card (port/build/states/memcard_staterun.mcr),
never the player's card. Next to each state its input history from power-on is written (slotN.input.json), as the viewer's F1..F4 do.

  python port/tools/make_test_states.py
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
BUILD = PORT / 'build'
STATES = BUILD / 'states'
EXE = BUILD / 'win' / 'fft_native.exe'
CFG = BUILD / 'winrun' / 'make_states.cfg'
SESSION = BUILD / 'native' / 'ls' / 'session_20261003_1717.txt'
RAMZA_AUTO = 0x80190a84                                                    # battle_stats_t[0] +0x1b8: auto_battle_setting (0 manual, 0xc auto)


def run(lines):
    CFG.parent.mkdir(parents=True, exist_ok=True)
    CFG.write_text('\n'.join(lines) + '\n')
    env = dict(os.environ)
    repo = PORT.parent
    env['FFT_MOUNTS'] = ';'.join([f'/disc={repo}\\fft_decomp\\build\\extracted\\files', f'/disc.bin={repo}\\game\\Final Fantasy Tactics.bin',
                                  f'/run.cfg={CFG}', f'/states={STATES}', f'/shots={BUILD}\\shots'])
    r = subprocess.run([str(EXE)], env=env, capture_output=True, text=True, errors='replace')
    out = r.stdout + r.stderr
    if 'snapshot of frame' not in out:
        sys.exit('no state was written:\n' + out[-2000:])


def taps(start, end, cross=()):
    """[(frame, mask)]: a button press of 4 frames every 60 frames, Circle (0x20), or Cross (0x40) inside the `cross` ranges"""
    out, f = [], start
    while f < end:
        m = 0x40 if any(a <= f < b for a, b in cross) else 0x20
        out += [(f, m), (f + 4, 0)]
        f += 60
    return out


def main():
    if not EXE.exists():
        sys.exit(f'no {EXE}: build it first (port/native/win/build_win.py)')
    common = ['gpu 1', 'nativeonly 1', 'audio 1', 'memcard /states/memcard_staterun.mcr']
    (STATES / 'memcard_staterun.mcr').unlink(missing_ok=True)
    session = [(int(f), int(m, 16)) for f, m in (e.split(':') for e in SESSION.read_text().strip().strip(',').split(',') if e)]
    pads1 = [(f + 1, m) for f, m in session if f + 1 < 13000]               # the viewer logs the frame before the one the input applies to
    run([f'pad {f} {m}' for f, m in pads1] + ['frames 13000', 'snapsave 13000 /states/battle_start.state'] + common)
    pads2 = taps(13100, 33500)
    run(['snapload /states/battle_start.state', f'pokewhen {RAMZA_AUTO} {0x00cc0000} {RAMZA_AUTO} {0x00cc000c}']
        + [f'pad {f} {m}' for f, m in pads2] + ['frames 33500', 'snapsave 33500 /states/battle_end.state'] + common)
    pads3 = [p for p in taps(33600, 48000, cross=[(47000, 47600)]) if p[0] < 48000]
    run(['snapload /states/battle_end.state'] + [f'pad {f} {m}' for f, m in pads3] + ['frames 48000', 'snapsave 48000 /states/battle2_start.state'] + common)
    hist1 = [(f - 1, m) for f, m in pads1]
    hist2 = hist1 + [(f - 1, m) for f, m in pads2]
    hist3 = hist2 + [(f - 1, m) for f, m in pads3]
    for slot, (state, frame, hist) in {1: ('battle_start.state', 13000, hist1), 2: ('battle_end.state', 33500, hist2),
                                       3: ('battle2_start.state', 48000, hist3)}.items():
        shutil.copyfile(STATES / state, STATES / f'slot{slot}.state')
        (STATES / f'slot{slot}.input.json').write_text(json.dumps({'frame': frame, 'last': 0, 'card': None, 'inputs': hist,
                                                                    'note': 'made by port/tools/make_test_states.py'}))
        print(f'slot {slot}: {state} (frame {frame})')


if __name__ == '__main__':
    main()
