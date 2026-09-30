#!/usr/bin/env python3
r"""play.py -- play the NATIVE build of Final Fantasy Tactics in a window (prototype).

The native game (the decomp's C, compiled with a modern compiler, on the HLE of the SDK hardware layer and the software GPU) runs in the toolchain container; it streams every frame
to this program over a pipe and receives the controller state back; this window shows the frame and reads the keyboard. No sound yet (SPU/XA are not modelled), movies are skipped.

  python port/native/play.py [--hd 2|3|4] [--scale K] [--cfg FILE]        (Docker Desktop must be running; build first: .\port\native\lockstep.ps1 -BuildOnly -Scenario title -Gpu)
  .\port\native\play.ps1 [-Hd 2..4] [-WorldCheat] [-Verify]                (the same, builds when needed; -WorldCheat skips the first battle; -Verify runs the original code alongside and
                                                                            compares RAM and VRAM at every frame, stopping if they ever differ)

Keys:  arrows = d-pad   Z = Cross (cancel)   X = Circle (confirm)   A = Square   S = Triangle (menu)   Q/W = L1/R1   E/R = L2/R2   Enter = Start   Backspace = Select
       P = pause   Tab (hold) = fast forward   F12 = save a screenshot (port/build/shots/play-*.png)
       F1..F4 = save the whole machine state to slot 1..4   F5..F8 = load slot 1..4   (files in port/build/states; a state is valid for the program build that wrote it)
Title screen: press Enter, then X a few times (the intro movies are skipped).
"""
import argparse
import os
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path

from PIL import Image, ImageTk

ROOT = Path(__file__).resolve().parents[1]                  # port/
REPO = ROOT.parent / 'fft_decomp'
BIN = ROOT.parent / 'game' / 'Final Fantasy Tactics.bin'
VOL = os.environ.get('FFT_LS_VOL', 'fft-ls-objs')

PAD_KEYS = {'Up': 0x1000, 'Right': 0x2000, 'Down': 0x4000, 'Left': 0x8000, 'Return': 0x800, 'BackSpace': 0x100, 'z': 0x40, 'x': 0x20, 'a': 0x80, 's': 0x10,
            'q': 0x04, 'w': 0x08, 'e': 0x01, 'r': 0x02, 'space': 0x800}


def docker_cmd(cfg_path, name):
    """The container command line: the driver in play mode with the pipes as stdin / stdout (frames out, controller + commands in)."""
    states = ROOT / 'build' / 'states'
    states.mkdir(parents=True, exist_ok=True)
    return ['docker', 'run', '--rm', '-i', '--pull=never', '--name', name, '--cap-add', 'SYS_RAWIO', '-e', 'PLAY=1',
            '--volume', f'{ROOT}:/port', '--volume', f'{VOL}:/ob', '--volume', f'{REPO}\\build\\extracted\\files:/disc:ro', '--volume', f'{BIN}:/disc.bin:ro',
            '--volume', f'{cfg_path}:/run.cfg:ro', '--volume', f'{states}:/states', 'fft-decomp-dev:local', 'sh', '/port/native/build_run_lockstep.sh']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hd', type=int, default=0, help='render the display buffers at 2..4 times the resolution (polygons on the finer grid)')
    ap.add_argument('--scale', type=int, default=3, help='window magnification of 1x frames (HD frames are shown as they are)')
    ap.add_argument('--fps', type=float, default=60.0, help='frame rate cap')
    ap.add_argument('--cfg', help='a run.cfg for the driver (default: "frames 0", [hd N], "play 2"; play.ps1 builds one with the cheats) -- it must end with "play 1" (verified) or "play 2" (native only)')
    args = ap.parse_args()

    if args.cfg:
        cfg_path = Path(args.cfg).resolve()
    else:
        cfg = ['frames 0'] + ([f'hd {min(args.hd, 4)}'] if args.hd >= 2 else []) + ['play 2']
        cfg_path = ROOT / 'build' / 'native' / 'ls' / 'play.cfg'
        cfg_path.parent.mkdir(parents=True, exist_ok=True)
        cfg_path.write_text('\n'.join(cfg) + '\n')
    shots = ROOT / 'build' / 'shots'
    shots.mkdir(parents=True, exist_ok=True)
    log_path = ROOT / 'build' / 'native' / 'ls' / 'play.log'
    name = f'fft-play-{os.getpid()}'
    proc = subprocess.Popen(docker_cmd(cfg_path, name), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)

    state = {'frame': None, 'closed': False, 'paused': False, 'last_rgb': None, 'err': '', 'cmd': 0}
    lock = threading.Lock()
    pressed = set()

    def read_exact(n):
        chunks = []
        while n:
            b = proc.stdout.read(n)
            if not b:
                return None
            chunks.append(b)
            n -= len(b)
        return b''.join(chunks)

    def reader():
        while True:
            hdr = read_exact(8)
            if hdr is None or hdr[:2] != b'FR':
                break
            w, h, n = struct.unpack('<HHH', hdr[2:8])
            data = read_exact(w * h * 3)
            if data is None:
                break
            with lock:
                state['frame'] = (w, h, n, data)
        state['closed'] = True

    def err_reader():
        with open(log_path, 'w', encoding='utf-8') as log:
            for line in iter(proc.stderr.readline, b''):
                t = line.decode('utf-8', 'replace').rstrip()
                log.write(t + '\n')
                log.flush()
                if any(k in t for k in ('FRAME', 'CRASH', 'HANG', 'differs', 'stopping', 'loads a code overlay', 'poke', 'NATIVE')):
                    print(t, file=sys.stderr)
                    state['err'] = t[:120]

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=err_reader, daemon=True).start()

    root = tk.Tk()
    root.title('FFT native build -- Z/X = cancel/confirm, arrows, Enter = Start, P = pause, Tab = fast forward, F1-F4 save state, F5-F8 load, F12 = screenshot')
    label = tk.Label(root, bg='black')
    label.pack()
    info = tk.Label(root, text='booting the game (a few seconds) ...', anchor='w', font=('Consolas', 9))
    info.pack(fill='x')
    photo = {'img': None}
    last = {'send': time.time(), 't0': time.time(), 'frames': 0, 'shot': 0}

    def key_down(e):
        k = e.keysym.lower() if len(e.keysym) == 1 else e.keysym
        if k == 'p' and 'p' not in pressed:
            state['paused'] = not state['paused']
            if not state['paused']:
                root.after(1, send_pad)
        elif k in ('F1', 'F2', 'F3', 'F4'):
            state['cmd'] = int(k[1:])                                   # save slot n
            info.config(text=f'saving state {k[1:]} ...')
        elif k in ('F5', 'F6', 'F7', 'F8'):
            state['cmd'] = 0x10 + int(k[1:]) - 4                        # load slot n
            info.config(text=f'loading state {int(k[1:]) - 4} ...')
        elif k == 'F12':
            with lock:
                rgb = state['last_rgb']
            if rgb:
                last['shot'] += 1
                p = shots / f'play-{int(time.time())}-{last["shot"]}.png'
                Image.frombytes('RGB', rgb[:2], rgb[2]).save(p)
                info.config(text=f'saved {p}')
        pressed.add(k)

    def key_up(e):
        pressed.discard(e.keysym.lower() if len(e.keysym) == 1 else e.keysym)

    root.bind('<KeyPress>', key_down)
    root.bind('<KeyRelease>', key_up)

    def pad_mask():
        m = 0
        for k in pressed:
            m |= PAD_KEYS.get(k, 0)
        return m

    def send_pad():
        if state['paused']:
            return
        try:
            proc.stdin.write(struct.pack('<HB', pad_mask(), state['cmd']))
            state['cmd'] = 0
            proc.stdin.flush()
        except OSError:
            state['closed'] = True
            return
        last['send'] = time.time()
        root.after(1, poll)

    def poll():
        if state['closed']:
            info.config(text='the game stopped: ' + (state['err'] or 'see the console / port/build/native/ls/play.log'))
            return
        with lock:
            fr = state['frame']
            state['frame'] = None
        if fr is None:
            root.after(1, poll)
            return
        w, h, n, data = fr
        state['last_rgb'] = (w, h, data)
        img = Image.frombytes('RGB', (w, h), data)
        if w <= 400:
            img = img.resize((w * args.scale, h * args.scale), Image.NEAREST)
        photo['img'] = ImageTk.PhotoImage(img)
        label.config(image=photo['img'])
        last['frames'] += 1
        if last['frames'] % 30 == 0:
            now = time.time()
            info.config(text=f'frame {n}   {30 / (now - last["t0"]):.1f} fps   {w}x{h}' + ('   [paused]' if state['paused'] else ''))
            last['t0'] = now
        delay = 0.0 if 'Tab' in pressed else max(0.0, last['send'] + 1.0 / args.fps - time.time())
        root.after(int(delay * 1000), send_pad)

    def on_close():
        try:
            proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            subprocess.run(['docker', 'kill', name], capture_output=True)
            proc.kill()
        root.destroy()

    root.protocol('WM_DELETE_WINDOW', on_close)
    root.focus_force()
    root.after(10, poll)
    root.mainloop()


if __name__ == '__main__':
    main()
