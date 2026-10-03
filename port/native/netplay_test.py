#!/usr/bin/env python3
"""netplay_test.py -- two players, two native game instances, input-delay lockstep over a loopback socket (headless; Python standard library only).

  python port/native/netplay_test.py [--frames 14000] [--delay 6] [--hotseat 0x1e] [--seat2 0x4] [--jitter-ms 0]

Player 1 (the host, controller 1) plays the title menu and every turn the seat rules give to controller 1; player 2 (the joiner, controller 2) plays the turns of the units in --seat2.
Both run the game in native-only mode; `hotseat` makes the AI allies of the first battle player-controlled so that there are turns for player 2. Only controller values cross the
(loopback) connection, plus a state hash every 60 frames for desync detection. Passes when both instances report identical state hashes for every sampled frame.
"""
import argparse
import bisect
import os
import random
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import netplay  # noqa: E402
import play     # noqa: E402  (docker_cmd, ROOT)


def make_script(seed, start, end, title=False):
    """Controller script as sorted (frame, buttons): the START / CIRCLE presses that lead from the title into the first battle (optional), then random play like padgen.ps1."""
    rng = random.Random(seed)
    entries = []
    if title:
        for f in range(600, 1180, 40):
            entries.append((f, 0x800 if ((f - 600) // 40) % 2 == 0 else 0x20))
            entries.append((f + 6, 0))
    dpad = [0x1000, 0x2000, 0x4000, 0x8000]
    f = start
    while f < end:
        r = rng.random()
        if r < 0.36:
            b = rng.choice(dpad)
        elif r < 0.60:
            b = 0x20
        elif r < 0.70:
            b = 0x40
        elif r < 0.76:
            b = 0x10
        elif r < 0.80:
            b = 0x80
        elif r < 0.86:
            b = rng.choice([0x04, 0x08])
        elif r < 0.89:
            b = 0x800
        elif r < 0.91:
            b = 0x100
        elif r < 0.94:
            b = rng.choice([0x01, 0x02])
        elif r < 0.97:
            b = rng.choice(dpad) | 0x20
        else:
            b = 0
        entries.append((f, b))
        f += rng.randint(2, 11)
        entries.append((f, 0))
        f += rng.randint(1, 7)
    entries.sort(key=lambda e: e[0])
    frames = [e[0] for e in entries]

    def value_at(frame):
        i = bisect.bisect_right(frames, frame) - 1
        return entries[i][1] if i >= 0 else 0
    value_at.entries = entries
    return value_at


class Player(threading.Thread):
    def __init__(self, idx, sock, cfg_text, frames, delay, script, jitter_ms):
        super().__init__(daemon=True)
        self.idx = idx
        self.name = f'fft-netplay-{os.getpid()}-{idx}'
        self.ls = netplay.Lockstep(sock, idx == 0, delay)
        self.proc = subprocess.Popen(play.docker_cmd(play.make_session(self.name, cfg_text), self.name), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.frames = frames
        self.script = script
        self.jitter = jitter_ms / 1000.0
        self.log = []
        self.error = None
        self.last_n = 0
        self.turns = []
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        for line in iter(self.proc.stderr.readline, b''):
            t = line.decode('utf-8', 'replace').rstrip()
            if t.startswith('H '):
                _, n, h = t.split()
                self.ls.report_hash(int(n), int(h, 16))
            else:
                self.log.append(t)

    def _read(self, n):
        chunks = []
        while n:
            b = self.proc.stdout.read(n)
            if not b:
                raise EOFError('the driver closed the frame stream')
            chunks.append(b)
            n -= len(b)
        return b''.join(chunks)

    def run(self):
        rng = random.Random(1000 + self.idx)
        try:
            while self.last_n < self.frames:
                hdr = self._read(8)
                w, h, n = struct.unpack('<HHH', hdr[2:8])
                if w * h:
                    self._read(w * h * 3)
                self.last_n = n
                if self.jitter:
                    time.sleep(rng.random() * self.jitter)
                local = self.script(n + 1)
                t0 = time.time()
                while True:
                    r = self.ls.step(n, local)
                    if r is not None:
                        break
                    if time.time() - t0 > 30:
                        raise TimeoutError(f'player {self.idx + 1}: the other controller for frame {n + 1} never arrived')
                    time.sleep(0.0005)
                self.proc.stdin.write(struct.pack('<HHB', r[0], r[1], 0))
                self.proc.stdin.flush()
        except Exception as e:                     # noqa: BLE001
            self.error = repr(e)
        finally:
            try:
                self.proc.stdin.close()
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames', type=int, default=14000)
    ap.add_argument('--delay', type=int, default=6)
    ap.add_argument('--hotseat', default='0x1e')
    ap.add_argument('--seat2', default='0x4')
    ap.add_argument('--jitter-ms', type=float, default=0.0)
    ap.add_argument('--random-battle', action='store_true', help='the first battle draws a random encounter from ENTD sets 1-59 (cheat: g_battle_entd_selection_mode = 3)')
    ap.add_argument('--dump-cfg', help='do not play: write a run.cfg that replays exactly the controller values this test would apply (both seats, shifted by the input delay), for lockstep.ps1 -CfgFile')
    args = ap.parse_args()

    cheat = ['pokewhen 0x800459dc 0 0x800459dc 3 1'] if args.random_battle else []
    cfg = ['frames 0', 'seats 2', f'seat2units {args.seat2}', f'hotseat {args.hotseat}', 'hashevery 60', 'noframes 1'] + cheat + ['play 2']
    cfg_text = '\n'.join(cfg) + '\n'

    end = args.frames + 100
    s1, s2 = make_script(11, 1180, end, title=True), make_script(22, 1180, end)
    if args.dump_cfg:                                          # the game frame g plays with script(g - delay): the entries move by the delay
        lines = [f'frames {args.frames}', 'seats 2', f'seat2units {args.seat2}', f'hotseat {args.hotseat}', 'gpu 1'] + (['pokewhen 0x800459dc 0 0x800459dc 3 1'] if args.random_battle else [])
        lines += [f'pad {f + args.delay} {b}' for f, b in s1.entries] + [f'pad2 {f + args.delay} {b}' for f, b in s2.entries]
        Path(args.dump_cfg).write_text('\n'.join(lines) + '\n')
        print(f'wrote {args.dump_cfg}: {len(s1.entries)} + {len(s2.entries)} controller entries')
        return
    import socket
    srv = socket.socket()
    srv.bind(('127.0.0.1', 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    box = {}
    t = threading.Thread(target=lambda: box.update(conn=srv.accept()[0]))
    t.start()
    cli = socket.create_connection(('127.0.0.1', port))
    t.join()
    for s in (cli, box['conn']):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    p1 = Player(0, box['conn'], cfg_text, args.frames, args.delay, s1, args.jitter_ms)
    p2 = Player(1, cli, cfg_text, args.frames, args.delay, s2, args.jitter_ms)
    t0 = time.time()
    p1.start()
    p2.start()
    p1.join()
    p2.join()
    dt = time.time() - t0
    time.sleep(1.0)
    for p in (p1, p2):
        print(f'player {p.idx + 1}: {p.last_n} frames, {len(p.ls.my_hash)} state hashes, desync {p.ls.desync}, error {p.error}')
        for line in p.log:
            if 'plays now' in line or 'CRASH' in line or 'FRAME' in line:
                print('   ', line[:150])
    common = sorted(set(p1.ls.my_hash) & set(p2.ls.my_hash))
    same = all(p1.ls.my_hash[f] == p2.ls.my_hash[f] for f in common)
    print(f'{len(common)} common state-hash samples, identical: {same};  {args.frames / dt:.0f} frames/s per player (delay {args.delay} frames, jitter {args.jitter_ms} ms)')
    ok = same and common and not p1.error and not p2.error and p1.ls.desync is None and p2.ls.desync is None
    print('NETPLAY LOCKSTEP OK' if ok else 'NETPLAY LOCKSTEP FAILED')
    for p in (p1, p2):
        subprocess.run(['docker', 'kill', p.name], capture_output=True)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
