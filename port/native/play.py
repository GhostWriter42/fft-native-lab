#!/usr/bin/env python3
r"""play.py -- play the NATIVE build of Final Fantasy Tactics in a window (prototype).

The native game (the decomp's C, compiled with a modern compiler, on the HLE of the SDK hardware layer and the software GPU) runs in the toolchain container; it streams every frame
to this program over a pipe and receives the controller state back; this window shows the frame and reads the keyboard. No sound yet (SPU/XA are not modelled), movies are skipped.

  python port/native/play.py [--hd 2|3|4] [--scale K] [--cfg FILE]        (Docker Desktop must be running; build first: .\port\native\lockstep.ps1 -BuildOnly -Scenario title -Gpu)
  .\port\native\play.ps1 [-Hd 2..4] [-WorldCheat] [-Verify]                (the same, builds when needed; -WorldCheat skips the first battle; -Verify runs the original code alongside and
                                                                            compares RAM and VRAM at every frame, stopping if they ever differ)

Two players (input-delay lockstep, see netplay.py; every player runs their own copy of the game and only the controllers cross the network):
  python port/native/play.py --host 7777            wait for the other player, then start together           (you are player 1, controller 1)
  python port/native/play.py --invite 7777          start playing alone now; when the other player connects they are taken into the game in progress
  python port/native/play.py --join HOST:7777       connect to either                                          (you are player 2, controller 2)

Keys:  arrows = d-pad   Z = Cross (cancel)   X = Circle (confirm)   A = Square   S = Triangle (menu)   Q/W = L1/R1   E/R = L2/R2   Enter = Start   Backspace = Select
       P = pause   Tab (hold) = fast forward   F12 = save a screenshot (port/build/shots/play-*.png)
       F1..F4 = save the whole machine state to slot 1..4   F5..F8 = load slot 1..4   (files in port/build/states; a state is valid for the program build that wrote it)
Title screen: press Enter, then X a few times (the intro movies are skipped).
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path

from PIL import Image, ImageTk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import netplay  # noqa: E402
from audio_out import AudioOut  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]                  # port/
REPO = ROOT.parent / 'fft_decomp'
BIN = ROOT.parent / 'game' / 'Final Fantasy Tactics.bin'
VOL = os.environ.get('FFT_LS_VOL', 'fft-ls-objs')
STATES = ROOT / 'build' / 'states'

PAD_KEYS = {'Up': 0x1000, 'Right': 0x2000, 'Down': 0x4000, 'Left': 0x8000, 'Return': 0x800, 'BackSpace': 0x100, 'z': 0x40, 'x': 0x20, 'a': 0x80, 's': 0x10,
            'q': 0x04, 'w': 0x08, 'e': 0x01, 'r': 0x02, 'space': 0x800}
SLOT_SYNC = 9                                               # the state slot used to take a second player into a game in progress


def make_session(name, cfg_text):
    """A directory with the run configuration (mounted at /session; the driver rereads it whenever a state is loaded, which is how a second player is added to a running game)."""
    d = ROOT / 'build' / 'session' / name
    d.mkdir(parents=True, exist_ok=True)
    (d / 'run.cfg').write_text(cfg_text)
    return d


def docker_cmd(session_dir, name):
    """The container command line: the driver in play mode with the pipes as stdin / stdout (frames out, controller + commands in)."""
    STATES.mkdir(parents=True, exist_ok=True)
    return ['docker', 'run', '--rm', '-i', '--pull=never', '--name', name, '--cap-add', 'SYS_RAWIO', '-e', 'PLAY=1',
            '--volume', f'{ROOT}:/port', '--volume', f'{VOL}:/ob', '--volume', f'{REPO}\\build\\extracted\\files:/disc:ro', '--volume', f'{BIN}:/disc.bin:ro',
            '--volume', f'{session_dir}:/session', '--volume', f'{STATES}:/states', 'fft-decomp-dev:local', 'sh', '/port/native/build_run_lockstep.sh']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hd', type=int, default=0, help='render the display buffers at 2..4 times the resolution (polygons on the finer grid)')
    ap.add_argument('--scale', type=int, default=3, help='window magnification of 1x frames (HD frames are shown as they are)')
    ap.add_argument('--fps', type=float, default=60.0, help='frame rate cap')
    ap.add_argument('--cfg', help='a run.cfg for the driver (default: "frames 0", [hd N], "play 2"; play.ps1 builds one with the cheats) -- it must end with "play 1" (verified) or "play 2" (native only)')
    ap.add_argument('--host', type=int, metavar='PORT', help='two players: wait for the other player on this TCP port, then start (you are player 1: controller 1)')
    ap.add_argument('--invite', type=int, metavar='PORT', help='two players: start now, listen on this TCP port, and take the other player into the game in progress when they connect (player 1)')
    ap.add_argument('--join', metavar='HOST:PORT', help='two players: connect to the host (you are player 2: controller 2)')
    ap.add_argument('--delay', type=int, default=6, help='netplay input delay in frames (hides the network latency)')
    ap.add_argument('--hotseat', default='0x1e', help='two players (the host decides): battle unit slots made player-controlled, as a bit mask (default: the AI allies of the first battle)')
    ap.add_argument('--seat2', default='0x1a', help='two players (the host decides): battle unit slots that player 2 plays, as a bit mask')
    ap.add_argument('--random-battle', action='store_true', help='two players (the host decides): the first battle is a random encounter from ENTD sets 1-59 (cheat)')
    ap.add_argument('--mute', action='store_true', help='no sound')
    ap.add_argument('--test-frames', type=int, default=0, help='headless self-test: no window is shown, the title-to-battle controller script is played, the program exits after N frames and prints a summary')
    args = ap.parse_args()

    name = f'fft-play-{os.getpid()}'
    STATES.mkdir(parents=True, exist_ok=True)

    def net_cfg():
        cheat = ['pokewhen 0x800459dc 0 0x800459dc 3 1'] if args.random_battle else []
        return '\n'.join(['frames 0', 'seats 2', f'seat2units {args.seat2}', f'hotseat {args.hotseat}', 'hashevery 60', 'audio 1'] + cheat + ([f'hd {min(args.hd, 4)}'] if args.hd >= 2 else []) + ['play 2']) + '\n'

    net = None                                              # the lockstep link once a second player is in the game
    need_load = False                                       # the joiner of a game in progress: load the host's state at the first frame
    inv = {'phase': 'off', 'conn': None, 'saved': None}     # --invite: off | idle | saving | net
    if args.join:
        h, _, port = args.join.rpartition(':')
        net = netplay.Lockstep(netplay.join(h or '127.0.0.1', int(port)), False, args.delay)
        got = net.wait_blob([b'C'])
        if not got:
            sys.exit('the host did not send its configuration')
        cfg_text = got[1][1].decode()
        got = net.wait_blob([b'S', b'N'])
        if not got:
            sys.exit('the host did not say how to start')
        if got[0] == b'S':                                  # joining a game in progress: the host's whole machine state
            (STATES / f'slot{SLOT_SYNC}.state').write_bytes(got[1][1])
            need_load = True
        print('connected; the host chose this configuration:', cfg_text.replace('\n', ' | '), '(state of frame %d received)' % got[1][0] if need_load else '')
    elif args.host:
        cfg_text = net_cfg()
        print(f'waiting for the other player on port {args.host} (they run: play.py --join <this machine>:{args.host}) ...')
        conn, addr = netplay.host(args.host)
        net = netplay.Lockstep(conn, True, args.delay)
        net.send_blob(b'C', 0, cfg_text.encode())
        net.send_blob(b'N', 0, b'')
        print(f'player 2 connected from {addr[0]}')
    elif args.cfg:
        cfg_text = Path(args.cfg).read_text()
    else:
        cfg_text = '\n'.join(['frames 0', 'audio 1'] + ([f'hd {min(args.hd, 4)}'] if args.hd >= 2 else []) + ['play 2']) + '\n'
    if args.invite:
        inv['phase'] = 'idle'
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('0.0.0.0', args.invite))
        srv.listen(1)

        def accept():
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f'player 2 connected from {addr[0]}: taking them into the game')
            inv['conn'] = conn
            srv.close()
        threading.Thread(target=accept, daemon=True).start()
        print(f'playing; the other player can join at any time: play.py --join <this machine>:{args.invite}')

    session = make_session(name, cfg_text)
    shots = ROOT / 'build' / 'shots'
    shots.mkdir(parents=True, exist_ok=True)
    log_path = ROOT / 'build' / 'native' / 'ls' / 'play.log'
    log_path.parent.mkdir(parents=True, exist_ok=True)
    audio = None if (args.mute or args.test_frames) else AudioOut()
    if audio is not None and not audio.ok:
        print('no audio device: playing without sound')
        audio = None
    proc = subprocess.Popen(docker_cmd(session, name), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)

    state = {'frame': None, 'closed': False, 'paused': False, 'last_rgb': None, 'err': '', 'cmd': 0, 'n': 0, 'who': 1}
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
            hdr = read_exact(10)
            if hdr is None or hdr[:2] != b'FR':
                break
            w, h, n, ab = struct.unpack('<HHHH', hdr[2:10])
            data = read_exact(w * h * 3)
            if data is None:
                break
            if ab:
                snd = read_exact(ab)
                if snd is None:
                    break
                if audio:
                    audio.write(snd)
            with lock:
                state['frame'] = (w, h, n, data)
        state['closed'] = True

    def err_reader():
        with open(log_path, 'w', encoding='utf-8') as log:
            for line in iter(proc.stderr.readline, b''):
                t = line.decode('utf-8', 'replace').rstrip()
                log.write(t + '\n')
                log.flush()
                m = re.search(r'controller (\d) plays now', t)
                if m:
                    state['who'] = int(m.group(1))                          # which controller the game listens to now (two-player mode)
                m = re.search(r'snapshot of frame (\d+) written to /states/slot%d\.state' % SLOT_SYNC, t)
                if m:
                    inv['saved'] = int(m.group(1))                          # the host's state for the joining player is on disk
                if net and t.startswith('H '):
                    _, hn, hh = t.split()
                    net.report_hash(int(hn), int(hh, 16))                   # this instance's state hash, for the desync check against the other player's
                    continue
                if any(k in t for k in ('FRAME', 'CRASH', 'HANG', 'differs', 'stopping', 'loads a code overlay', 'poke', 'NATIVE')):
                    print(t, file=sys.stderr)
                    state['err'] = t[:120]

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=err_reader, daemon=True).start()

    root = tk.Tk()
    if args.test_frames:
        root.withdraw()
    root.title('FFT native build -- Z/X = cancel/confirm, arrows, Enter = Start, P = pause, Tab = fast forward, F1-F4 save state, F5-F8 load, F12 = screenshot')
    label = tk.Label(root, bg='black')
    label.pack()
    info = tk.Label(root, text='booting the game (a few seconds) ...', anchor='w', font=('Consolas', 9))
    info.pack(fill='x')
    photo = {'img': None}
    last = {'send': time.time(), 't0': time.time(), 'frames': 0, 'shot': 0, 'start': time.time()}

    def key_down(e):
        k = e.keysym.lower() if len(e.keysym) == 1 else e.keysym
        if k == 'p' and 'p' not in pressed:
            state['paused'] = not state['paused']
            if not state['paused']:
                root.after(1, send_pad)
        elif k in ('F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8') and (net or inv['phase'] not in ('off', 'idle')):
            info.config(text='saving / loading states is disabled in two-player games (it would desynchronise the two games)')
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

    def test_pad(n):                                                    # the START / CIRCLE presses from the title menu into the first battle (padgen.ps1)
        if 600 <= n < 1180 and (n - 600) % 40 < 6:
            return 0x800 if ((n - 600) // 40) % 2 == 0 else 0x20
        return 0

    def pad_mask():
        if args.test_frames:
            return 0 if (net and not net.is_host) else test_pad(state['n'] + 1)
        m = 0
        for k in pressed:
            m |= PAD_KEYS.get(k, 0)
        return m

    def answer(pad, cmd, two=False):
        """The reply to a reported frame: the controller (and the second controller in two-player mode) and a command for the driver (0 none, 1..8 save slot, 0x11..0x1f load slot)."""
        proc.stdin.write(struct.pack('<HHB', pad, 0, cmd) if two else struct.pack('<HB', pad, cmd))
        proc.stdin.flush()

    def send_pad():
        nonlocal net, need_load
        if state['paused']:
            return
        try:
            if need_load:                                               # the joiner of a game in progress: the host's state replaces the freshly booted game
                need_load = False
                answer(0, 0x10 + SLOT_SYNC, two=True)
            elif inv['phase'] == 'idle' and inv['conn'] is not None:    # a second player has connected: ask the driver for a state of the game as it is now
                inv['phase'] = 'saving'
                answer(pad_mask(), SLOT_SYNC)
            elif inv['phase'] == 'saving':
                if inv['saved'] is None:                                # (the driver's "written" message is still on its way)
                    root.after(5, send_pad)
                    return
                text = net_cfg()
                data = (STATES / f'slot{SLOT_SYNC}.state').read_bytes()
                (session / 'run.cfg').write_text(text)                  # the driver rereads it when it loads the state below, and so changes into a two-seat game
                link = netplay.Lockstep(inv['conn'], True, args.delay)
                link.send_blob(b'C', 0, text.encode())
                link.send_blob(b'S', inv['saved'], data)
                net = link
                inv['phase'] = 'net'
                answer(pad_mask(), 0x10 + SLOT_SYNC)                    # ... the host loads the same state, so both games continue from identical memory
            elif net:                                                   # the controllers of the next frame: mine (delayed) and the other player's
                r = net.step(state['n'], pad_mask())
                if r is None:
                    if net.closed:                                      # a lockstep game cannot go on without the other controller
                        info.config(text='the other player left the game: stopped at frame %d' % state['n'])
                        if args.test_frames:
                            print(f'test: the other player left at game frame {state["n"]}, desync {net.desync}, state hashes compared equal: {net.checked}')
                            on_close()
                        return
                    info.config(text=f'frame {state["n"]}: waiting for the other player ...')
                    root.after(2, send_pad)
                    return
                proc.stdin.write(struct.pack('<HHB', r[0], r[1], 0))
                proc.stdin.flush()
            else:
                answer(pad_mask(), state['cmd'])
                state['cmd'] = 0
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
        state['n'] = n
        state['last_rgb'] = (w, h, data)
        img = Image.frombytes('RGB', (w, h), data)
        if w <= 400:
            img = img.resize((w * args.scale, h * args.scale), Image.NEAREST)
        photo['img'] = ImageTk.PhotoImage(img)
        label.config(image=photo['img'])
        last['frames'] += 1
        if args.test_frames and last['frames'] >= args.test_frames:
            dt = time.time() - last['start']
            shown = last['frames']
            print(f'test: {shown} frames in {dt:.1f} s ({shown / dt:.1f} fps), game frame {n}, two players: {bool(net)}, desync {net.desync if net else None}, state hashes compared equal: {net.checked if net else 0}')
            on_close()
            return
        if last['frames'] % 30 == 0:
            now = time.time()
            me = 1 if (net and net.is_host) or inv['phase'] != 'off' else 2
            who = '' if not net else ('   you are player %d (%s)   controller %d plays now%s' % (me, 'host' if me == 1 else 'guest', state['who'], '  <- YOUR TURN' if state['who'] == me else ''))
            bad = '' if not net or net.desync is None else f'   DESYNC at frame {net.desync[0]}!'
            info.config(text=f'frame {n}   {30 / (now - last["t0"]):.1f} fps   {w}x{h}' + who + bad + ('   [paused]' if state['paused'] else ''))
            last['t0'] = now
        delay = 0.0 if ('Tab' in pressed and not net) else max(0.0, last['send'] + 1.0 / args.fps - time.time())
        root.after(int(delay * 1000), send_pad)

    def on_close():
        if audio:
            audio.close()
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
