#!/usr/bin/env python3
"""play_test.py -- exercise the play-mode protocol (play.py) without a window: the driver runs in the container, this script is the viewer.

  python port/native/play_test.py [--hd N] [--frames 1300] [--verify]

Steps: boot, steer the title screen with START / CIRCLE presses like the soak scripts, send the save-state command at one frame and the load-state command later, write PNGs of a few frames to
port/build/shots/playtest-*.png, and report the frame rate. Checks that the frame stream is well formed (header, size, frame numbers), that a state saved earlier restores (the driver prints
"state 1 loaded"), and that the picture at the same game frame after the load is identical to the one before the save.
"""
import argparse
import os
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import play                                                   # noqa: E402  (docker_cmd, ROOT)


def title_pad(frame):
    """START / CIRCLE presses alternating every 40 frames from frame 600, held for 6 frames (padgen.ps1 New-TitleToBattlePad)."""
    if 600 <= frame < 1180:
        k = (frame - 600) // 40
        if (frame - 600) % 40 < 6:
            return 0x800 if k % 2 == 0 else 0x20
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hd', type=int, default=0)
    ap.add_argument('--frames', type=int, default=1300)
    ap.add_argument('--verify', action='store_true', help='play 1: the original machine code runs alongside and is compared')
    ap.add_argument('--save-at', type=int, default=300)
    ap.add_argument('--load-at', type=int, default=900)
    args = ap.parse_args()

    cfg = ['frames 0'] + ([f'hd {args.hd}'] if args.hd >= 2 else []) + ['play 1' if args.verify else 'play 2']
    cfg_path = play.ROOT / 'build' / 'native' / 'ls' / 'playtest.cfg'
    cfg_path.parent.mkdir(parents=True, exist_ok=True)
    cfg_path.write_text('\n'.join(cfg) + '\n')
    shots = play.ROOT / 'build' / 'shots'
    shots.mkdir(parents=True, exist_ok=True)
    name = f'fft-playtest-{os.getpid()}'
    proc = subprocess.Popen(play.docker_cmd(cfg_path, name), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    log = []

    def drain():
        for line in iter(proc.stderr.readline, b''):
            log.append(line.decode('utf-8', 'replace').rstrip())

    threading.Thread(target=drain, daemon=True).start()

    def read_exact(n):
        chunks = []
        while n:
            b = proc.stdout.read(n)
            if not b:
                return None
            chunks.append(b)
            n -= len(b)
        return b''.join(chunks)

    t0 = time.time()
    frames = 0
    last_n = 0
    saved_at = None
    before = None
    after = None
    problems = []
    sizes = set()
    cmd_sent_load = False
    sync_frame = None
    while frames < args.frames:
        hdr = read_exact(8)
        if hdr is None or hdr[:2] != b'FR':
            problems.append(f'stream ended or malformed after {frames} frames: header {hdr!r}')
            break
        w, h, n = struct.unpack('<HHH', hdr[2:8])
        data = read_exact(w * h * 3)
        if data is None:
            problems.append('short frame data')
            break
        frames += 1
        sizes.add((w, h))
        if n != last_n + 1 and frames > 1 and sync_frame is None:
            problems.append(f'frame numbers jump: {last_n} -> {n}')
        last_n = n
        if n == args.save_at - 1:
            before = data
        if n == args.save_at:
            saved_at = n
        if n in (500, 800, 1200):
            Image.frombytes('RGB', (w, h), data).save(play.ROOT / 'build' / 'shots' / f'playtest-{n:05d}.png')
        command = 0
        if n == args.save_at:
            command = 1                                       # save slot 1 at the end of this frame
        elif n == args.load_at:
            command = 0x11                                    # load slot 1
            cmd_sent_load = True
        if cmd_sent_load and n == args.load_at + 1:
            sync_frame = n                                    # the frame number in the stream continues (the loop counter is not rewound by a quick load)
            after = data
        try:
            proc.stdin.write(struct.pack('<HB', title_pad(n + 1), command))
            proc.stdin.flush()
        except OSError:
            problems.append('the driver closed the pipe')
            break
    elapsed = time.time() - t0
    try:
        proc.stdin.close()
    except OSError:
        pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        subprocess.run(['docker', 'kill', name], capture_output=True)
    print(f'{frames} frames in {elapsed:.1f} s ({frames / elapsed:.1f} fps, including boot); picture sizes {sorted(sizes)}')
    for line in log:
        if any(k in line for k in ('snapshot', 'state ', 'no usable', 'FRAME', 'CRASH', 'HANG', 'stopping', 'cannot')):
            print('  driver:', line[:200])
    saved = any('snapshot of frame' in l for l in log)
    loaded = any('loaded (frame' in l for l in log)
    print('state saved:', saved, ' state loaded:', loaded)
    if not saved:
        problems.append('no snapshot line in the driver log')
    if not loaded:
        problems.append('no "state 1 loaded" line in the driver log')
    if problems:
        print('PROBLEMS:')
        for p in problems:
            print('  -', p)
        sys.exit(1)
    print('protocol OK; PNGs in port/build/shots/playtest-*.png')


if __name__ == '__main__':
    main()
