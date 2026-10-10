"""scene.py -- replayable scenes: recorded gameplay as regression tests.

A scene (port/scenes/NAME.json) is the controller input from power-on up to its last frame (`inputs`: [frame, mask] changes, the viewer's
log format), the frames that matter (`start`..`end`) and, once accepted, the expected pictures at its checkpoints (`shots`: frame -> hash of the
picture's pixels). It holds no game data, so it survives rebuilds and can be committed; a scene that starts from a memory card names the card's
image at power-on (`card`, a file in port/build/states, kept private).

Record one in the GPU viewer (`play.ps1 -Gl`): F9 starts, F9 ends (also across F5..F8 state loads: the viewer keeps the input history from power-on).

  python port/tools/scene.py list
  python port/tools/scene.py run NAME [--native] [--every 600]   replay it: lockstep against the original machine code (or the native game alone),
                                                                 pictures at the checkpoints compared with the accepted ones
  python port/tools/scene.py accept NAME                         run it and store its pictures as the expected ones
  python port/tools/scene.py from-input NAME FILE START END      make a scene from a logged session (port/build/native/ls/session_*.txt / last_input.txt)

Runs one Docker container at a time.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
SCENES = PORT / 'scenes'
STATES = PORT / 'build' / 'states'
SHOTS = PORT / 'build' / 'shots'
LOCKSTEP = PORT / 'native' / 'lockstep.ps1'


def load(name):
    p = SCENES / f'{name}.json'
    if not p.exists():
        sys.exit(f'no scene {p}')
    return p, json.loads(p.read_text(encoding='utf-8'))


def checkpoints(doc, every):
    start, end = doc['start'], doc['end']
    pts = {start, end} | set(range(start - start % every + every, end, every))
    return sorted(p for p in pts if 1 <= p <= end)


def picture_hash(png):
    from PIL import Image
    im = Image.open(png).convert('RGB')
    return hashlib.sha1(im.tobytes() + repr(im.size).encode()).hexdigest()[:16]


def run(name, native, every):
    path, doc = load(name)
    pts = checkpoints(doc, every)
    cfg = [f'frames {doc["end"]}', 'gpu 1']
    if native:
        cfg.append('nativeonly 1')
    card = STATES / 'scene_card.mcr'
    card.unlink(missing_ok=True)                                                # no card file: a blank, formatted card
    if doc.get('card'):
        src = STATES / doc['card']
        if not src.exists():
            sys.exit(f'the scene starts from the memory card {src}, which is not on this machine')
        shutil.copyfile(src, card)
    cfg.append('memcard /states/scene_card.mcr')
    cfg += [f'pad {f + 1} {m}' for f, m in doc['inputs']]                      # the viewer logs the frame BEFORE the one the input applies to
    cfg += [f'shot {p}' for p in pts]
    run_cfg = PORT / 'build' / f'scene_{name}.cfg'
    run_cfg.write_text('\n'.join(cfg) + '\n')
    for p in pts:
        (SHOTS / f'f{p:06d}.png').unlink(missing_ok=True)
    print(f'scene {name}: {doc["end"]} frames ({"native game alone" if native else "lockstep against the original code"}), {len(pts)} checkpoints')
    # (a build without the soak harness's -Scenario title shortcut: scenes are recorded in the play build, which boots the game normally)
    r = subprocess.run(['powershell', '-NoProfile', '-File', str(LOCKSTEP), '-CfgFile', str(run_cfg)], capture_output=True, text=True, errors='replace')
    out = r.stdout + r.stderr
    verdict = [l for l in out.splitlines() if l.startswith('==') or 'DIVERGE' in l or 'differ' in l.lower() or 'CRASH' in l or 'HANG' in l]
    lockstep_ok = native or any('identical at every VSync' in l for l in verdict)
    got = {}
    for p in pts:
        png = SHOTS / f'f{p:06d}.png'
        got[str(p)] = picture_hash(png) if png.exists() else None
    return path, doc, got, lockstep_ok, verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
    for c in ('run', 'accept'):
        s = sub.add_parser(c)
        s.add_argument('name')
        s.add_argument('--native', action='store_true', help='the native game alone (faster, no comparison with the original code)')
        s.add_argument('--every', type=int, default=600, help='a checkpoint picture every N frames (default 600)')
    s = sub.add_parser('from-input')
    s.add_argument('name'); s.add_argument('file'); s.add_argument('start', type=int); s.add_argument('end', type=int)
    s.add_argument('--card', default=None, help='the memory card at power-on (a file in port/build/states)')
    s.add_argument('--description', default='')
    a = ap.parse_args()

    if a.cmd == 'list':
        for p in sorted(SCENES.glob('*.json')):
            d = json.loads(p.read_text(encoding='utf-8'))
            print(f'{p.stem:32s} frames {d["start"]:>6}..{d["end"]:<6} {"accepted" if d.get("shots") else "not accepted":13s} {d.get("description", "")}')
        return
    if a.cmd == 'from-input':
        text = Path(a.file).read_text(encoding='ascii').strip().strip(',')
        inputs = [[int(f), int(m, 16)] for f, m in (e.split(':') for e in text.split(',') if e)]
        SCENES.mkdir(parents=True, exist_ok=True)
        doc = {'name': a.name, 'description': a.description, 'start': a.start, 'end': a.end, 'card': a.card,
               'inputs': [e for e in inputs if e[0] <= a.end], 'shots': {}}
        (SCENES / f'{a.name}.json').write_text(json.dumps(doc, indent=1), encoding='utf-8')
        print(f'port/scenes/{a.name}.json: {len(doc["inputs"])} input changes, frames {a.start}..{a.end}')
        return

    path, doc, got, lockstep_ok, verdict = run(a.name, a.native, a.every)
    for l in verdict[-3:]:
        print('  ' + l)
    missing = [f for f, h in got.items() if h is None]
    if missing:
        print(f'  no picture at frames {", ".join(missing)} (the run ended early?)')
    if a.cmd == 'accept':
        if not lockstep_ok or missing:
            sys.exit('not accepted: the run did not complete cleanly')
        doc['shots'] = got
        path.write_text(json.dumps(doc, indent=1), encoding='utf-8')
        print(f'accepted: {len(got)} pictures stored in {path.name}')
        return
    want = doc.get('shots') or {}
    if not want:
        print('  (not accepted yet: no pictures to compare; `accept` stores them)')
    changed = [f for f in want if got.get(f) != want[f]]
    for f in changed:
        print(f'  picture at frame {f} changed: port/build/shots/f{int(f):06d}.png')
    ok = lockstep_ok and not missing and not changed
    print(f'scene {a.name}: {"PASS" if ok else "FAIL"}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
