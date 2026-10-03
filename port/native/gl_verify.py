#!/usr/bin/env python3
r"""gl_verify.py -- does the GPU renderer draw what the game's software GPU model draws?

Runs the native game in the container with `gltrace 2` (every frame: the GPU command trace AND the software model's picture of the same frame), replays each trace with
gl_renderer.GLRenderer at 1x on the graphics card (hidden window), and compares the two pictures pixel by pixel. The title-menu -> first-battle controller script is played.

    port\build\venv\Scripts\python.exe port\native\gl_verify.py [--frames 1500] [--world-cheat] [--shots 5]

Per 100 frames it prints the fraction of pixels that differ by more than a tolerance and the mean error; at the end the worst frames are written as side-by-side PNGs
(software | GPU | amplified difference) to port\build\shots. Differences are expected only where the software model and a GPU rasteriser legitimately disagree (edge pixels of
polygons, 5-bit rounding of blends), not in whole sprites or colours.
"""
import argparse
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

import glfw
import moderngl
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gl_renderer import GLRenderer  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent / 'fft_decomp'
BIN = ROOT.parent / 'game' / 'Final Fantasy Tactics.bin'
VOL = os.environ.get('FFT_LS_VOL', 'fft-ls-objs')


def read_exact(f, n):
    chunks = []
    while n:
        b = f.read(n)
        if not b:
            return None
        chunks.append(b)
        n -= len(b)
    return b''.join(chunks)


def test_pad(n):
    if 600 <= n < 1180 and (n - 600) % 40 < 6:
        return 0x800 if ((n - 600) // 40) % 2 == 0 else 0x20
    return 0


def make_pad(seed):
    import random
    rng = random.Random(seed)
    held = {'mask': 0, 'until': 0}

    def pad(n):
        if n < 1180 or not seed:
            return test_pad(n)
        if n >= held['until']:
            held['mask'] = rng.choice([0x20, 0x20, 0x20, 0x40, 0x1000, 0x2000, 0x4000, 0x8000, 0x10, 0x800, 0, 0, 0x04, 0x08])
            held['until'] = n + rng.choice([3, 6, 6, 10, 20])
        return held['mask'] if (n % 12) < 6 else 0
    return pad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames', type=int, default=1500)
    ap.add_argument('--world-cheat', action='store_true', help='skip the first battle (go to the world map)')
    ap.add_argument('--random-battle', action='store_true')
    ap.add_argument('--tol', type=int, default=24, help='per-channel difference (0..255) above which a pixel counts as different')
    ap.add_argument('--shots', type=int, default=4, help='how many of the worst frames to write out')
    ap.add_argument('--save', default='', help='frame numbers (comma separated) whose software | GPU | difference pictures are always written')
    ap.add_argument('--pad-seed', type=int, default=0, help='random play from frame 1180 (seeded) instead of the fixed title-to-battle presses only')
    ap.add_argument('--vram-check', action='store_true', help='also receive the whole VRAM of the software model each frame (gltrace 3) and report where the 1x VRAM mirror of the renderer differs outside the framebuffer areas')
    ap.add_argument('--fb-check', action='store_true', help='with --vram-check: report the first frame at which the GPU framebuffer (whole 1x VRAM area) differs from the software VRAM')
    ap.add_argument('--fb-min', type=int, default=0, help='with --fb-check: only report a frame with more than this many differing pixels')
    ap.add_argument('--scale', type=int, default=1, help='render at this internal resolution and compare the picture averaged down to 1x (flat areas must match; polygon edges differ a little)')
    ap.add_argument('--flicker', action='store_true', help='report windows of frames where the SOFTWARE picture alternates (A B A B ...): flashing that is in the game itself')
    ap.add_argument('--script', default='', help='controller script frame:mask,frame:mask,... (a mask holds until the next entry) used INSTEAD of the built-in presses')
    ap.add_argument('--cfg-extra', default='', help='more run.cfg lines separated by ;')
    args = ap.parse_args()

    cfg = ['frames 0', 'gltrace 3' if args.vram_check else 'gltrace 2']
    if args.world_cheat:
        cfg.append('pokewhen 0x800960e4 0x27 0x800960e4 0x3b 1')                      # g_battle_game_state 0x27 -> 0x3b: the battle is over, on to the world map
    if args.random_battle:
        cfg.append('pokewhen 0x800459dc 0 0x800459dc 3 1')
    if args.cfg_extra:
        cfg += args.cfg_extra.split(';')
    cfg.append('play 2')
    name = f'fft-glverify-{os.getpid()}'
    session = ROOT / 'build' / 'session' / name
    session.mkdir(parents=True, exist_ok=True)
    (session / 'run.cfg').write_text('\n'.join(cfg) + '\n')
    states = ROOT / 'build' / 'states'
    states.mkdir(parents=True, exist_ok=True)
    cmd = ['docker', 'run', '--rm', '-i', '--pull=never', '--name', name, '--cap-add', 'SYS_RAWIO', '-e', 'PLAY=1',
           '--volume', f'{ROOT}:/port', '--volume', f'{VOL}:/ob', '--volume', f'{REPO}\\build\\extracted\\files:/disc:ro', '--volume', f'{BIN}:/disc.bin:ro',
           '--volume', f'{session}:/session', '--volume', f'{states}:/states', 'fft-decomp-dev:local', 'sh', '/port/native/build_run_lockstep.sh']
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)

    if not glfw.init():
        sys.exit('glfw.init failed')
    glfw.window_hint(glfw.VISIBLE, glfw.FALSE)
    glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
    glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
    glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
    win = glfw.create_window(64, 64, 'gl_verify', None, None)
    glfw.make_context_current(win)
    ctx = moderngl.create_context()
    r = GLRenderer(ctx, args.scale)
    print('GPU:', ctx.info['GL_RENDERER'])

    pad_fn = make_pad(args.pad_seed)
    if args.script:
        entries = sorted((int(a_), int(b_, 0)) for a_, _, b_ in (it.partition(':') for it in args.script.split(',') if it.strip()))

        def pad_fn(n, entries=entries):
            m = 0
            for f_, v_ in entries:
                if f_ <= n:
                    m = v_
            return m
    dump_frames = {int(v) for v in args.save.split(',') if v.strip()}
    vram_reports = []
    hist_sw = []
    flick = []
    fb_first = None
    results = []          # (frame, frac, mean, None, None): metrics only; the pictures of the worst frames and of --save frames are kept in `kept` (memory stays flat)
    kept = {}
    want_frames = {int(v) for v in args.save.split(',') if v.strip()}
    t0 = time.time()
    n = 0
    gl_time = 0.0
    while n < args.frames:
        hdr = read_exact(proc.stdout, 10)
        if hdr is None:
            print('the game stopped')
            break
        if hdr[:2] != b'GL':
            print('unexpected packet', hdr[:2])
            break
        frame, ab, tb = struct.unpack('<HHI', hdr[2:10])
        trace = read_exact(proc.stdout, tb)
        if ab:
            read_exact(proc.stdout, ab)
        fr = read_exact(proc.stdout, 10)
        if fr is None or fr[:2] != b'FR':
            print('missing the software frame')
            break
        w, h, fn, fab = struct.unpack('<HHHH', fr[2:10])
        sw = np.frombuffer(read_exact(proc.stdout, w * h * 3), np.uint8).reshape(h, w, 3)
        if fab:
            read_exact(proc.stdout, fab)
        if n in dump_frames:
            (ROOT / 'build' / 'shots' / f'trace-{n}.bin').write_bytes(trace)
        swvram = None
        if args.vram_check:
            swvram = np.frombuffer(read_exact(proc.stdout, 1024 * 512 * 2), np.uint16).reshape(512, 1024)
        g0 = time.time()
        r.run(trace)
        gl = r.read_display_rgb()
        if args.scale > 1:
            S_ = args.scale
            gh, gw = gl.shape[0] // S_, gl.shape[1] // S_
            gl = np.rint(gl.reshape(gh, S_, gw, S_, 3).mean(axis=(1, 3))).astype(np.uint8)
        gl_time += time.time() - g0
        if gl.shape == sw.shape:
            d = np.abs(gl.astype(np.int16) - sw.astype(np.int16))
            frac = float((d.max(axis=2) > args.tol).mean())
            mean = float(d.mean())
        else:
            frac, mean = 1.0, 255.0
        results.append((n, frac, mean, None, None))
        if args.flicker:
            hist_sw.append(sw.astype(np.int16)[::2, ::2].copy())
            if len(hist_sw) > 3:
                hist_sw.pop(0)
            if len(hist_sw) == 3 and hist_sw[0].shape == hist_sw[1].shape == hist_sw[2].shape:
                d1 = float(np.abs(hist_sw[2] - hist_sw[1]).mean()); d2 = float(np.abs(hist_sw[2] - hist_sw[0]).mean())
                flick.append((n, d1, d2, float(hist_sw[2].mean())))
        if n in want_frames or len(kept) < args.shots or frac > min((kept[k][0] for k in kept if k not in want_frames), default=-1.0):
            kept[n] = (frac, mean, sw.copy(), gl.copy())
            extra = [k for k in kept if k not in want_frames]
            if len(extra) > args.shots:
                del kept[min(extra, key=lambda k: kept[k][0])]
        if swvram is not None and args.fb_check and fb_first is None:
            full = np.frombuffer(r.fbo.read(viewport=(0, 0, 1024, 512), components=3), np.uint8).reshape(512, 1024, 3).astype(np.int16)
            c5 = np.stack([swvram & 31, (swvram >> 5) & 31, (swvram >> 10) & 31], axis=2).astype(np.int16)
            ref = (c5 << 3) | (c5 >> 2)
            dd = np.abs(full - ref).max(axis=2) > args.tol
            dd[:, 256:] = False                                    # the two 256x240 framebuffers of the game (x 0..255, y 0..479); the rest of the VRAM holds textures that the HD texture does not mirror
            dd[480:, :] = False
            if int(dd.sum()) > args.fb_min:
                ys, xs = np.nonzero(dd)
                fb_first = n
                print(f'  FIRST framebuffer difference (whole 1x VRAM area, tolerance {args.tol}) at frame {n}: {int(dd.sum())} pixels, bbox x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}; fb rects {r.fb_rects}')
                (ROOT / 'build' / 'shots' / f'trace-{n}.bin').write_bytes(trace)
                np.savez_compressed(ROOT / 'build' / 'shots' / f'fbdiff-{n}.npz', full=full.astype(np.uint8), ref=ref.astype(np.uint8))
        if swvram is not None:
            bad = r.vram != swvram
            for (rx, ry, rw, rh) in r.fb_rects:
                bad[ry:ry + rh, rx:rx + rw] = False
            nb = int(bad.sum())
            if nb and len(vram_reports) < 12:
                ys, xs = np.nonzero(bad)
                tiles = sorted({(int(x) // 64 * 64, int(y) // 64 * 64) for x, y in zip(xs, ys)})
                vram_reports.append((n, nb, tiles[:8]))
                print(f'  VRAM mirror differs outside the framebuffer rects at frame {n}: {nb} pixels, bbox x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}, 64x64 tiles {tiles[:6]}')
        n += 1
        proc.stdin.write(struct.pack('<HB', pad_fn(n), 0))
        proc.stdin.flush()
        if n % 100 == 0:
            blk = results[-100:]
            print(f'frames {n - 99}-{n}: pixels differing > {args.tol}: mean {np.mean([b[1] for b in blk]) * 100:.3f}%  worst {max(b[1] for b in blk) * 100:.2f}%  mean error {np.mean([b[2] for b in blk]):.3f}   '
                  f'({n / (time.time() - t0):.1f} frames/s overall, GPU replay {1000 * gl_time / n:.2f} ms/frame, {r.stats["tris"] // max(1, r.stats["frames"])} triangles/frame)')
    try:
        proc.stdin.close()
        proc.wait(timeout=5)
    except Exception:
        subprocess.run(['docker', 'kill', name], capture_output=True)
    glfw.terminate()
    if not results:
        sys.exit(1)
    if args.flicker and flick:
        fl = np.array(flick)
        isf = (fl[:, 1] > 6.0) & (fl[:, 2] < 0.35 * fl[:, 1])        # this frame differs from the last one but resembles the one before: A B A
        print(f'flicker: {int(isf.sum())} frames look like A B A B in {len(fl)}')
        for w0 in range(0, len(fl), 200):
            blk = isf[w0:w0 + 200]
            if blk.sum() > 20:
                print(f'  frames {int(fl[w0, 0])}-{int(fl[min(w0 + 199, len(fl) - 1), 0])}: {int(blk.sum())} of {len(blk)} frames alternate (mean brightness {fl[w0:w0 + 200, 3].mean():.0f})')
    fr = np.array([x[1] for x in results])
    print(f'\n{len(results)} frames compared: {100 * (fr < 0.001).mean():.1f}% of frames have < 0.1% differing pixels; mean {100 * fr.mean():.3f}%, worst {100 * fr.max():.2f}% (frame {int(np.argmax(fr))}); '
          f'trace overflows: {r.stats["overflow"]}')
    shots = ROOT / 'build' / 'shots'
    shots.mkdir(parents=True, exist_ok=True)
    want = {int(v) for v in args.save.split(',') if v.strip()}
    order = sorted(kept, key=lambda k: -kept[k][0])
    for k in order:
        f = k
        frac, mean, sw, gl = kept[k]
        if gl.shape != sw.shape:
            continue
        diff = np.clip(np.abs(gl.astype(np.int16) - sw.astype(np.int16)) * 4, 0, 255).astype(np.uint8)
        img = np.concatenate([sw, gl, diff], axis=1)
        Image.fromarray(img).resize((img.shape[1] * 2, img.shape[0] * 2), Image.NEAREST).save(shots / f'glverify-{f}.png')
        np.savez_compressed(shots / f'glverify-{f}.npz', sw=sw, gl=gl)
        print(f'  frame {f}: {frac * 100:.2f}% pixels differ -> port/build/shots/glverify-{f}.png (software | GPU | difference x4)')


if __name__ == '__main__':
    main()
