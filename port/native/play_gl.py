#!/usr/bin/env python3
r"""play_gl.py -- play the NATIVE build of Final Fantasy Tactics with the graphics card doing the drawing.

The game runs in the toolchain container; instead of finished pictures it sends, every frame, its GPU command trace (run.cfg `gltrace 1`); gl_renderer.py replays that on the GPU
(OpenGL) at 1x-4x the original resolution into this window. The software GPU model in the container still runs (it is the reference the lockstep tests use) but at the original
resolution, which is cheap. Sound goes through Windows audio as in play.py.

  port\build\venv\Scripts\python.exe port\native\play_gl.py [--scale 2] [--cfg FILE] [--mute] [--fullscreen] [--test-frames N]
  .\port\native\play.ps1 -Gl [-Hd 1..4] [-WorldCheat]                                  (builds when needed; -Hd is the internal resolution factor here, default 2)

Keys:  arrows = d-pad   Z = Cross (cancel)   X = Circle (confirm)   A = Square   S = Triangle (menu)   Q/W = L1/R1   E/R = L2/R2   Enter = Start   Backspace = Select
       T = texture filter on / off (EPX, at --scale 2 and more)
       F9 = start / end a SCENE (the controller input from power-on up to here goes to port/scenes/: a replayable regression test, see port/tools/scene.py)
       P = pause   Tab (hold) = fast forward   F1..F4 = save the machine state to slot 1..4   F5..F8 = load slot 1..4   F11 = full screen   F12 = screenshot   Esc = quit
Title screen: press Enter, then X a few times (the intro movies are skipped).
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import glfw
import moderngl
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gl_renderer import GLRenderer  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]                  # port/
REPO = ROOT.parent / 'fft_decomp'
BIN = ROOT.parent / 'game' / 'Final Fantasy Tactics.bin'
VOL = os.environ.get('FFT_LS_VOL', 'fft-ls-objs')
STATES = ROOT / 'build' / 'states'

GAMEPAD_BUTTONS = {glfw.GAMEPAD_BUTTON_A: 0x40, glfw.GAMEPAD_BUTTON_B: 0x20, glfw.GAMEPAD_BUTTON_X: 0x80, glfw.GAMEPAD_BUTTON_Y: 0x10,
                   glfw.GAMEPAD_BUTTON_LEFT_BUMPER: 0x04, glfw.GAMEPAD_BUTTON_RIGHT_BUMPER: 0x08, glfw.GAMEPAD_BUTTON_BACK: 0x100, glfw.GAMEPAD_BUTTON_START: 0x800,
                   glfw.GAMEPAD_BUTTON_DPAD_UP: 0x1000, glfw.GAMEPAD_BUTTON_DPAD_RIGHT: 0x2000, glfw.GAMEPAD_BUTTON_DPAD_DOWN: 0x4000, glfw.GAMEPAD_BUTTON_DPAD_LEFT: 0x8000}
KEYS = {glfw.KEY_UP: 0x1000, glfw.KEY_RIGHT: 0x2000, glfw.KEY_DOWN: 0x4000, glfw.KEY_LEFT: 0x8000, glfw.KEY_ENTER: 0x800, glfw.KEY_KP_ENTER: 0x800, glfw.KEY_BACKSPACE: 0x100,
        glfw.KEY_Z: 0x40, glfw.KEY_X: 0x20, glfw.KEY_A: 0x80, glfw.KEY_S: 0x10, glfw.KEY_Q: 0x04, glfw.KEY_W: 0x08, glfw.KEY_E: 0x01, glfw.KEY_R: 0x02, glfw.KEY_SPACE: 0x800}


def fast_hold(win):
    return glfw.get_key(win, glfw.KEY_TAB) == glfw.PRESS


def read_exact(f, n):
    chunks = []
    while n:
        b = f.read(n)
        if not b:
            return None
        chunks.append(b)
        n -= len(b)
    return b''.join(chunks)


SCRIPT = []


def test_pad(n):
    if SCRIPT:
        m = 0
        for f, v in SCRIPT:
            if f <= n:
                m = v
        return m                                            # the START / CIRCLE presses from the title menu into the first battle (padgen.ps1)
    if 600 <= n < 1180 and (n - 600) % 40 < 6:
        return 0x800 if ((n - 600) // 40) % 2 == 0 else 0x20
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--scale', type=int, default=2, help='internal resolution: 1 = the original, 2..4 = polygons and sprites drawn at that multiple (the HD framebuffer is 1024*S x 512*S)')
    ap.add_argument('--cfg', help='a run.cfg for the driver; it must contain `gltrace 1` and end with `play 2` (default: a minimal one)')
    ap.add_argument('--mute', action='store_true')
    ap.add_argument('--fullscreen', action='store_true')
    ap.add_argument('--filter', choices=['none', 'epx'], default='none', help='texture filter at --scale 2..4: epx = EPX / Scale2x on the game texels (smoother sprites and text); T toggles it')
    ap.add_argument('--smooth', action='store_true', help='bilinear filtering when the picture is scaled to the window')
    ap.add_argument('--script', default='', help='with --test-frames: controller script `frame:mask,frame:mask,...` (a mask holds until the next entry; e.g. 1100:0x800,1106:0) played INSTEAD of the title-to-battle presses')
    ap.add_argument('--replay', default='', help='with --test-frames: replay a logged session (the file written to port/build/native/ls/last_input.txt by an earlier run)')
    ap.add_argument('--trace-at', default='', help='with --test-frames: frame numbers whose GPU command trace is written to port/build/shots/trace-<frame>.bin')
    ap.add_argument('--vram-at', default='', help='with --test-frames: frame numbers whose 1x VRAM mirror (1024x512 uint16) is saved to port/build/shots/vram-<frame>.npy')
    ap.add_argument('--shot-at', default='', help='with --test-frames: frame numbers (comma separated) whose picture is written to port/build/shots/playgl-test-<frame>.png')
    ap.add_argument('--compare', action='store_true', help='diagnostic: the container also sends the picture of the software renderer for every frame; it is compared with the GPU picture live, frames that differ are saved')
    ap.add_argument('--record', action='store_true', help='diagnostic: keep the last ~4 seconds of frames in memory and write them to port/build/shots/record-<time>/ when flashing is detected, on F10 and on exit')
    ap.add_argument('--pace', action='store_true', help='limit to 60 frames per second by the clock (for the headless test, where there is no vsync; with sound the audio queue already paces)')
    ap.add_argument('--test-audio', action='store_true', help='with --test-frames: play the sound too (default: muted in the headless test)')
    ap.add_argument('--frame-times', default='', help='diagnostic: write every frame\'s wall time (game + transfer, GPU replay; ms) and trace size to this CSV file')
    ap.add_argument('--fast-effects', action='store_true', help='ability effects at 60 fps instead of the original 15-30 (run.cfg fasteffects 1)')
    ap.add_argument('--load-slot', type=int, default=0, metavar='N', help='start from the machine state in slot N (port/build/states/slotN.state; F1..F4 save, F5..F8 load)')
    ap.add_argument('--native-exe', nargs='?', const=str(ROOT / 'build' / 'win' / 'fft_native.exe'), default=None,
                    help='run the Windows build of the game (port/native/win/build_win.py) instead of the container: no Docker needed')
    ap.add_argument('--two', action='store_true', help='two players on this computer: controller 2 is the second gamepad; in battle the units in --seat2 are played with it')
    ap.add_argument('--host', type=int, metavar='PORT', help='two players over the network: wait for the other player on this TCP port, then start together (you are player 1)')
    ap.add_argument('--join', metavar='HOST:PORT', help='two players over the network: connect to the host (you are player 2)')
    ap.add_argument('--delay', type=int, default=6, help='network play: input delay in frames (hides the latency)')
    ap.add_argument('--hotseat', default='0x1e', help='two players: battle unit slots made player-controlled, as a bit mask (default: the AI allies of the first battle)')
    ap.add_argument('--seat2', default='0x1a', help='two players: battle unit slots that player 2 plays, as a bit mask')
    ap.add_argument('--test-frames', type=int, default=0, help='headless self-test: hidden window, the title-to-battle script, exit after N frames and print a summary')
    args = ap.parse_args()
    S = max(1, min(4, args.scale))
    for item in args.script.split(','):
        if item.strip():
            f_, _, m_ = item.partition(':')
            SCRIPT.append((int(f_), int(m_, 0)))
    if args.replay:
        for item in Path(args.replay).read_text().split(','):
            if item.strip():
                f_, _, m_ = item.partition(':')
                SCRIPT.append((int(f_) + 1, int(m_, 0)))        # logged at the answer after frame f = the controller of frame f + 1
    vram_at = {int(v) for v in args.vram_at.split(',') if v.strip()}
    trace_at = {int(v) for v in args.trace_at.split(',') if v.strip()}
    shot_at = {int(v) for v in args.shot_at.split(',') if v.strip()}

    name = f'fft-playgl-{os.getpid()}'
    if args.cfg:
        cfg_text = Path(args.cfg).read_text()
        if 'gltrace' not in cfg_text:
            cfg_text = cfg_text.replace('play 2', 'gltrace 1\nplay 2')
        if args.compare:
            cfg_text = cfg_text.replace('gltrace 1', 'gltrace 2')
    else:
        cfg_text = 'frames 0\naudio 1\nmemcard /states/memcard0.mcr\ngltrace %d\nplay 2\n' % (2 if args.compare else 1)
    two_seats = bool(args.two or args.host or args.join)
    net = None
    if args.join:                                                               # the host decides the configuration
        import netplay
        h_, _, port_ = args.join.rpartition(':')
        print(f'connecting to {h_ or "127.0.0.1"}:{port_} ...')
        net = netplay.Lockstep(netplay.join(h_ or '127.0.0.1', int(port_)), False, args.delay)
        got = net.wait_blob([b'C'])
        if not got:
            sys.exit('the host did not send its configuration')
        cfg_text = got[1][1].decode()
        if not net.wait_blob([b'N']):
            sys.exit('the host did not start the game')
        print('connected: you are player 2 (controller 2)')
    elif two_seats:
        extra = f'seats 2\nseat2units {args.seat2}\nhotseat {args.hotseat}\n' + ('hashevery 60\n' if args.host else '')
        cfg_text = cfg_text.replace('play 2', extra + 'play 2')
        if args.host:                                                           # both games must start from identical memory: a blank card (saves are not kept in network play)
            cfg_text = re.sub(r'^memcard .*\n', '', cfg_text, flags=re.M)
            import netplay
            print(f'waiting for the other player on port {args.host} (they run: play_gl.py --join <this computer>:{args.host}) ...')
            conn, addr = netplay.host(args.host)
            net = netplay.Lockstep(conn, True, args.delay)
            net.send_blob(b'C', 0, cfg_text.encode())
            net.send_blob(b'N', 0, b'')
            print(f'player 2 connected from {addr[0]}: you are player 1')
    if args.fast_effects and 'fasteffects' not in cfg_text:
        cfg_text = cfg_text.replace('play 2', 'fasteffects 1\nplay 2')
    if args.load_slot:                                                          # start from a saved machine state (it must come from this build of the program)
        if not (STATES / f'slot{args.load_slot}.state').exists():
            sys.exit(f'there is no state in slot {args.load_slot}')
        cfg_text = cfg_text.replace('play 2', f'snapload /states/slot{args.load_slot}.state\nplay 2')
    if args.test_frames:                                                        # headless tests never touch the player's memory card
        cfg_text = cfg_text.replace('memcard /states/memcard0.mcr', 'memcard /states/memcard_test.mcr')
    session = ROOT / 'build' / 'session' / name
    session.mkdir(parents=True, exist_ok=True)
    (session / 'run.cfg').write_text(cfg_text)
    STATES.mkdir(parents=True, exist_ok=True)
    shots = ROOT / 'build' / 'shots'
    shots.mkdir(parents=True, exist_ok=True)
    log_path = ROOT / 'build' / 'native' / 'ls' / 'play_gl.log'
    log_path.parent.mkdir(parents=True, exist_ok=True)

    audio = None
    if not args.mute and (args.test_audio or not args.test_frames):
        from audio_out import AudioOut
        audio = AudioOut()
        if not audio.ok:
            print('no audio device: playing without sound')
            audio = None

    env = None
    if args.native_exe:                                                         # the Windows build: the same pipes; FFT_MOUNTS stands in for the container's volumes
        env = dict(os.environ)
        env['FFT_MOUNTS'] = ';'.join([f'/disc={REPO}\\build\\extracted\\files', f'/disc.bin={BIN}', f'/session={session}', f'/states={STATES}',
                                      f'/shots={ROOT}\\build\\shots', f'/run.cfg={session}\\run.cfg'])
    cmd = [args.native_exe] if args.native_exe else ['docker', 'run', '--rm', '-i', '--pull=never', '--name', name, '--cap-add', 'SYS_RAWIO', '-e', 'PLAY=1',
           '--volume', f'{ROOT}:/port', '--volume', f'{VOL}:/ob', '--volume', f'{REPO}\\build\\extracted\\files:/disc:ro', '--volume', f'{BIN}:/disc.bin:ro',
           '--volume', f'{session}:/session', '--volume', f'{STATES}:/states', 'fft-decomp-dev:local', 'sh', '/port/native/build_run_lockstep.sh']
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, env=env)
    err = {'last': '', 'who': 1}

    def err_reader():
        with open(log_path, 'w', encoding='utf-8') as log:
            for line in iter(proc.stderr.readline, b''):
                t = line.decode('utf-8', 'replace').rstrip()
                log.write(t + '\n')
                log.flush()
                if net is not None and t.startswith('H '):
                    _, hn, hh = t.split()
                    net.report_hash(int(hn), int(hh, 16))                       # this game's state hash: compared with the other player's (desync check)
                    continue
                m_ = re.search(r'controller (\d) plays now', t)
                if m_:
                    err['who'] = int(m_.group(1))
                if any(k in t for k in ('FRAME', 'CRASH', 'HANG', 'differs', 'stopping', 'NATIVE', 'state ')):
                    print(t, file=sys.stderr)
                    err['last'] = t[:120]

    threading.Thread(target=err_reader, daemon=True).start()

    if not glfw.init():
        sys.exit('glfw.init failed')
    glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
    glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
    glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
    if args.test_frames:
        glfw.window_hint(glfw.VISIBLE, glfw.FALSE)
    win = glfw.create_window(1024, 768, 'FFT native build (GPU)', None, None)
    if not win:
        proc.kill()
        sys.exit('could not open an OpenGL 3.3 window')
    glfw.make_context_current(win)
    glfw.swap_interval(1)
    ctx = moderngl.create_context()
    rend = GLRenderer(ctx, S, smooth=args.smooth)
    filt = {'on': args.filter == 'epx'}
    rend.set_filter(1 if filt['on'] else 0)
    print(f'GPU: {ctx.info["GL_RENDERER"]}   internal resolution {256 * S}x{240 * S} (x{S})')

    state = {'paused': False, 'cmd': 0, 'n': 0, 'fs': False, 'down': set(), 'quit': False}
    saved_pos = {}

    def on_key(w, key, sc, action, mods):
        if action == glfw.PRESS:
            if key == glfw.KEY_ESCAPE:
                state['quit'] = True
            elif key == glfw.KEY_P:
                state['paused'] = not state['paused']
            elif glfw.KEY_F1 <= key <= glfw.KEY_F4:
                state['cmd'] = 1 + key - glfw.KEY_F1
                print(f'saving state {state["cmd"]}')
            elif glfw.KEY_F5 <= key <= glfw.KEY_F8:
                state['cmd'] = 0x11 + key - glfw.KEY_F5
                print(f'loading state {state["cmd"] - 0x10}')
            elif key == glfw.KEY_T:
                filt['on'] = not filt['on']
                rend.set_filter(1 if filt['on'] else 0)
                print('texture filter:', 'EPX' if filt['on'] else 'none')
            elif key == glfw.KEY_F9:
                state['scene_key'] = True
            elif key == glfw.KEY_F11:
                if not state['fs']:
                    saved_pos['p'] = glfw.get_window_pos(win) + glfw.get_window_size(win)
                    mon = glfw.get_primary_monitor()
                    vm = glfw.get_video_mode(mon)
                    glfw.set_window_monitor(win, mon, 0, 0, vm.size.width, vm.size.height, vm.refresh_rate)
                else:
                    x, y, w_, h_ = saved_pos['p']
                    glfw.set_window_monitor(win, None, x, y, w_, h_, 0)
                state['fs'] = not state['fs']
                glfw.swap_interval(1)
            elif key == glfw.KEY_F10 and args.record:
                dump_record('F10 pressed')
            elif key == glfw.KEY_F12:
                a = rend.read_display_rgb()
                p = shots / f'playgl-{int(time.time())}.png'
                Image.fromarray(a).save(p)
                print(f'saved {p}')

    glfw.set_key_callback(win, on_key)
    if args.fullscreen:
        on_key(win, glfw.KEY_F11, 0, glfw.PRESS, 0)

    def gamepad_mask(jid):                                  # positional, like the PS controller: A/B/X/Y = Cross/Circle/Square/Triangle
        if not glfw.joystick_is_gamepad(jid):
            return 0
        st = glfw.get_gamepad_state(jid)
        if not st:
            return 0
        m = 0
        for b, bit in GAMEPAD_BUTTONS.items():
            if st.buttons[b]:
                m |= bit
        ax = st.axes
        if ax[0] < -0.5:
            m |= 0x8000
        if ax[0] > 0.5:
            m |= 0x2000
        if ax[1] < -0.5:
            m |= 0x1000
        if ax[1] > 0.5:
            m |= 0x4000
        if ax[4] > 0.5:
            m |= 0x01                                       # left trigger = L2
        if ax[5] > 0.5:
            m |= 0x02                                       # right trigger = R2
        return m

    def pad_mask():
        if args.test_frames:
            return 0 if args.join else test_pad(state['n'] + 1)
        m = 0
        for k, bit in KEYS.items():
            if glfw.get_key(win, k) == glfw.PRESS:
                m |= bit
        return m | gamepad_mask(glfw.JOYSTICK_1)

    def pad2_mask():                                        # local two players: the second gamepad
        return 0 if args.test_frames else gamepad_mask(glfw.JOYSTICK_2)

    # ---- diagnostics (--compare / --record)
    import collections
    import numpy as np
    ring = collections.deque(maxlen=240)                      # (frame, pad, 1x picture)
    cmp_stat = {'frames': 0, 'bad': 0, 'worst': 0.0, 'saved': 0}
    flash = {'last_dump': 0.0}

    def small(a):
        return np.ascontiguousarray(a[::S, ::S]) if S > 1 else a

    def dump_record(why):
        if not ring:
            return
        d = shots / f'record-{time.strftime("%Y%m%d-%H%M%S")}'
        d.mkdir(parents=True, exist_ok=True)
        for k, (fr_, pad_, img) in enumerate(list(ring)[-120:]):
            Image.fromarray(img).save(d / f'{k:03d}_f{fr_}_pad{pad_:04x}.png')
        tail = ''
        try:
            tail = '\n'.join(log_path.read_text(encoding='utf-8', errors='replace').splitlines()[-30:])
        except OSError:
            pass
        (d / 'info.txt').write_text(f'{why}\nframes shown: {nframes_box[0]}\nscale {S}\n--- game log tail:\n{tail}\n', encoding='utf-8')
        print(f'recorded the last {min(120, len(ring))} frames to {d} ({why})')

    def flashing():
        """True when the last 30 frames alternate A B A B (this frame differs from the previous one but resembles the one before that)."""
        if len(ring) < 32:
            return False
        imgs = [r[2].astype(np.int16)[::2, ::2] for r in list(ring)[-31:]]
        if not (imgs[0].shape == imgs[1].shape == imgs[2].shape):
            return False
        n_alt = 0
        for i in range(2, len(imgs)):
            if imgs[i].shape != imgs[i - 1].shape or imgs[i].shape != imgs[i - 2].shape:
                continue
            d1 = float(np.abs(imgs[i] - imgs[i - 1]).mean())
            d2 = float(np.abs(imgs[i] - imgs[i - 2]).mean())
            if d1 > 4.0 and d2 < 0.35 * d1:
                n_alt += 1
        return n_alt >= 12

    nframes_box = [0]
    ft_rows, ft_last = [], [time.perf_counter()]
    # the player's sessions: last_input.txt plus a dated copy in sessions/ (headless test runs log elsewhere: a replay must never overwrite a real session)
    if args.test_frames:
        input_path = ROOT / 'build' / 'native' / 'ls' / 'test_input.txt'
    else:
        input_path = ROOT / 'build' / 'native' / 'ls' / 'last_input.txt'
        (ROOT / 'build' / 'native' / 'ls' / 'sessions').mkdir(parents=True, exist_ok=True)
    session_copy = None if args.test_frames else ROOT / 'build' / 'native' / 'ls' / 'sessions' / time.strftime('session_%Y%m%d_%H%M%S.txt')
    inp = {'last': 0, 'hist': [], 'pending_load': None, 'scene_start': None}  # hist: every controller change since POWER-ON as (frame, mask): exact even across state loads
    if args.load_slot:                                                          # a session started from a state continues that state's input history
        sj = STATES / f'slot{args.load_slot}.input.json'
        if sj.exists():
            import json as json0
            d0 = json0.loads(sj.read_text(encoding='utf-8'))
            inp['hist'] = [tuple(e) for e in d0['inputs']]
            inp['last'] = d0.get('last', 0)
    import json as json_

    def write_input_log():                                  # frame:mask,frame:mask,...  (the format of --script / --replay)
        text = ''.join(f'{f}:0x{m:x},' for f, m in inp['hist'])
        input_path.write_text(text, encoding='ascii')
        if session_copy is not None:
            session_copy.write_text(text, encoding='ascii')

    write_input_log()                                       # (empty: a new session)
    # the memory card as it was at power-on is part of what a replay needs (Continue / the save screen read it): keep a copy per session
    inp['card'] = None
    mc = re.search(r'^memcard /states/(\S+)', cfg_text, re.M)
    if mc and (STATES / mc.group(1)).exists():
        cards = STATES / 'cards'
        cards.mkdir(parents=True, exist_ok=True)
        inp['card'] = 'cards/' + time.strftime('session-%Y%m%d-%H%M%S.mcr')
        shutil.copyfile(STATES / mc.group(1), STATES / inp['card'])

    def slot_input_path(slot):
        return STATES / f'slot{slot}.input.json'

    def save_scene(end_frame):
        start = inp['scene_start']
        inp['scene_start'] = None
        scenes = ROOT / 'scenes'
        scenes.mkdir(parents=True, exist_ok=True)
        name = time.strftime('scene-%Y%m%d-%H%M%S')
        doc = {'name': name, 'description': '', 'start': start, 'end': end_frame, 'card': inp['card'],
               'inputs': [[f, m] for f, m in inp['hist'] if f <= end_frame], 'shots': {}}
        (scenes / f'{name}.json').write_text(json_.dumps(doc, indent=1), encoding='utf-8')
        print(f'scene saved: port/scenes/{name}.json (frames {start}..{end_frame}); give it a name and a description, then: python port/tools/scene.py accept {name}')
    t0 = time.time()
    tf = t0
    next_t = [t0]
    nframes = 0
    render_s = 0.0
    try:
        while not state['quit'] and not glfw.window_should_close(win):
            glfw.poll_events()
            if state['paused']:
                time.sleep(0.02)
                continue
            hdr = read_exact(proc.stdout, 10)
            if hdr is None or hdr[:2] != b'GL':
                print('the game stopped:', err['last'] or 'no error line')
                try:
                    print('--- last lines of the game log (port/build/native/ls/play_gl.log):')
                    for line_ in log_path.read_text(encoding='utf-8', errors='replace').splitlines()[-8:]:
                        print(line_)
                except OSError:
                    pass
                break
            frame, ab, tb = struct.unpack('<HHI', hdr[2:10])
            if args.frame_times:
                now_ft = time.perf_counter()
                ft_rows.append((frame, (now_ft - ft_last[0]) * 1000.0, tb))
                ft_last[0] = now_ft
            if inp['pending_load'] is not None:                     # a state was loaded (F5..F8): its input history becomes this session's
                slot, data = inp['pending_load']
                inp['pending_load'] = None
                if data and frame == data['frame'] + 1:
                    inp['hist'] = [tuple(e) for e in data['inputs']]
                    inp['last'] = data['last']
                    inp['card'] = data.get('card')
                    write_input_log()
                    print(f'state {slot}: input history restored (power-on to frame {data["frame"]})')
                elif data is None:
                    print(f'state {slot}: no input history next to it (saved by an older viewer): scenes recorded from here cannot be replayed')
            trace = read_exact(proc.stdout, tb)
            snd = read_exact(proc.stdout, ab) if ab else None
            if trace is None or (ab and snd is None):
                break
            if frame in trace_at:
                (shots / f'trace-{frame}.bin').write_bytes(trace)
            sw_img = None
            if args.compare:                                        # gltrace 2: the software renderer's picture of this frame follows
                fh = read_exact(proc.stdout, 10)
                if fh is None or fh[:2] != b'FR':
                    print('the game stopped (no software frame)')
                    break
                sw_w, sw_h, _, sw_ab = struct.unpack('<HHHH', fh[2:10])
                raw = read_exact(proc.stdout, sw_w * sw_h * 3)
                if sw_ab:
                    read_exact(proc.stdout, sw_ab)
                if raw is None:
                    break
                sw_img = np.frombuffer(raw, np.uint8).reshape(sw_h, sw_w, 3)
            if snd and audio:
                audio.wait_room(5)                                    # the audio clock paces the game: no chunk is ever dropped
                audio.write(snd)
            elif args.pace and not fast_hold(win):
                time.sleep(max(0.0, next_t[0] - time.time()))
                next_t[0] = max(next_t[0] + 1 / 60.0, time.time() - 0.1)
            g0 = time.time()
            rend.run(trace)
            fast = glfw.get_key(win, glfw.KEY_TAB) == glfw.PRESS and not args.test_frames
            if fast:
                glfw.swap_interval(0)
            w, h = glfw.get_framebuffer_size(win)
            if w and h and (not fast or nframes % 4 == 0):
                rend.present(ctx.screen, w, h)
                glfw.swap_buffers(win)
            if not fast:
                glfw.swap_interval(1)
            render_s += time.time() - g0
            state['n'] = frame
            nframes += 1
            if frame in vram_at:
                import numpy as np_
                np_.save(shots / f'vram-{frame}.npy', rend.vram)
            if frame in shot_at:
                Image.fromarray(rend.read_display_rgb()).save(shots / f'playgl-test-{frame}.png')
            pad = pad_mask()
            if pad != inp['last']:                                  # every change of the controller is logged: the session can be replayed exactly (--script)
                inp['last'] = pad
                inp['hist'].append((frame, pad))
                write_input_log()
            if state.get('scene_key'):
                state['scene_key'] = False
                if inp['scene_start'] is None:
                    inp['scene_start'] = frame
                    print(f'scene: recording from frame {frame} (F9 again to end it)')
                else:
                    save_scene(frame)
            if 1 <= state['cmd'] <= 4:                             # saving a state: keep the input history that produced it next to it
                slot_input_path(state['cmd']).write_text(json_.dumps({'frame': frame, 'last': inp['last'], 'card': inp['card'], 'inputs': inp['hist']}), encoding='utf-8')
            elif 0x11 <= state['cmd'] <= 0x14:
                sp = slot_input_path(state['cmd'] - 0x10)
                inp['pending_load'] = (state['cmd'] - 0x10, json_.loads(sp.read_text(encoding='utf-8')) if sp.exists() else None)
            nframes_box[0] = nframes
            if args.compare or args.record:
                gl_img = rend.read_display_rgb()
                if args.compare and sw_img is not None:
                    g1 = small(gl_img) if S == 1 else np.rint(gl_img.reshape(gl_img.shape[0] // S, S, gl_img.shape[1] // S, S, 3).mean(axis=(1, 3))).astype(np.uint8)
                    cmp_stat['frames'] += 1
                    if g1.shape == sw_img.shape:
                        frac = float((np.abs(g1.astype(np.int16) - sw_img.astype(np.int16)).max(axis=2) > 24).mean())
                    else:
                        frac = 1.0
                    cmp_stat['worst'] = max(cmp_stat['worst'], frac)
                    if frac > 0.01:
                        cmp_stat['bad'] += 1
                        if cmp_stat['saved'] < 12 and g1.shape == sw_img.shape:
                            cmp_stat['saved'] += 1
                            diff = np.clip(np.abs(g1.astype(np.int16) - sw_img.astype(np.int16)) * 4, 0, 255).astype(np.uint8)
                            Image.fromarray(np.concatenate([sw_img, g1, diff], axis=1)).save(shots / f'compare-f{frame}.png')
                            print(f'frame {frame}: the GPU picture differs from the software picture in {frac * 100:.1f}% of the pixels -> port/build/shots/compare-f{frame}.png (software | GPU | difference x4)')
                    if nframes % 300 == 0:
                        print(f'compare: {cmp_stat["frames"]} frames, {cmp_stat["bad"]} differ by more than 1% of the pixels, worst {cmp_stat["worst"] * 100:.2f}%')
                if args.record:
                    ring.append((frame, pad, small(gl_img)))
                    if nframes % 15 == 0 and time.time() - flash['last_dump'] > 10 and flashing():
                        flash['last_dump'] = time.time()
                        dump_record('flashing detected: the picture alternates frame by frame')
            if net is not None:                                     # network play: my controller goes out (delayed), both controllers of the next frame come back
                state['cmd'] = 0                                    # (no state saves / loads: the two games must stay identical)
                r_ = None
                while True:
                    r_ = net.step(frame, pad)
                    if r_ is not None:
                        break
                    if net.closed:
                        print(f'the other player left the game at frame {frame}' + (f' (DESYNC at frame {net.desync[0]})' if net.desync else ''))
                        state['quit'] = True
                        break
                    glfw.poll_events()
                    if state['quit'] or glfw.window_should_close(win):
                        break
                    time.sleep(0.001)
                if r_ is None:
                    break
                proc.stdin.write(struct.pack('<HHB', r_[0], r_[1], 0))
            elif two_seats:
                proc.stdin.write(struct.pack('<HHB', pad, pad2_mask(), state['cmd']))
            else:
                proc.stdin.write(struct.pack('<HB', pad, state['cmd']))
            proc.stdin.flush()
            state['cmd'] = 0
            if nframes % 60 == 0:
                now = time.time()
                who = ''
                if two_seats:
                    me = 2 if args.join else 1
                    who = f'   controller {err["who"]} plays now'
                    if net is not None:
                        who += f' (you are player {me}' + (', YOUR TURN)' if err['who'] == me else ')')
                        if net.desync:
                            who += f'   DESYNC at frame {net.desync[0]}!'
                glfw.set_window_title(win, f'FFT native build (GPU {S}x)   frame {frame}   {60 / (now - tf):.0f} fps   GPU {1000 * render_s / nframes:.2f} ms/frame' + who)
                tf = now
            if args.test_frames and nframes >= args.test_frames:
                dt = time.time() - t0
                print(f'test: {nframes} frames in {dt:.1f} s ({nframes / dt:.1f} fps), game frame {frame}, GPU replay {1000 * render_s / nframes:.2f} ms/frame'
                      + (f', two players over the network: desync {net.desync}, state hashes compared equal: {net.checked}' if net is not None else ''))
                break
    except Exception:                                         # never close silently: say why (and keep it in the log)
        import traceback
        tb = traceback.format_exc()
        print('the viewer failed:')
        print(tb)
        with open(log_path, 'a', encoding='utf-8') as lf:
            lf.write('viewer exception:\n' + tb)
    finally:
        if args.frame_times and ft_rows:
            with open(args.frame_times, 'w') as ftf:
                ftf.write('frame,ms,trace_bytes\n')
                for r_ in ft_rows:
                    ftf.write(f'{r_[0]},{r_[1]:.3f},{r_[2]}\n')
        if inp['scene_start'] is not None:
            save_scene(state['n'])
        print(f'your controller input of this session is in {input_path} (replay it with --script "$(contents)")')
        if args.record:
            dump_record('on exit')
        if args.compare:
            print(f'compare: {cmp_stat["frames"]} frames, {cmp_stat["bad"]} differ by more than 1% of the pixels, worst {cmp_stat["worst"] * 100:.2f}%')
        if audio:
            print(audio.stats())
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
        glfw.terminate()


if __name__ == '__main__':
    main()
