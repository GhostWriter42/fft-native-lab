"""launcher.pyw -- a small window to start the native build of Final Fantasy Tactics with the options of the GPU viewer (play_gl.py).

Double-click "Play FFT.bat" in the project folder (or: port\\build\\venv\\Scripts\\pythonw.exe port\\native\\launcher.pyw).
The choices are remembered in port/build/launcher.json.
"""
import json
import os
import subprocess
import sys
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

NATIVE = Path(__file__).resolve().parent
PORT = NATIVE.parent
BUILD = PORT / 'build'
STATES = BUILD / 'states'
EXE = BUILD / 'win' / 'fft_native.exe'
VENV_PY = BUILD / 'venv' / 'Scripts' / 'python.exe'
SETTINGS = BUILD / 'launcher.json'
NEW_CONSOLE = 0x00000010                                                   # subprocess.CREATE_NEW_CONSOLE

DEFAULTS = {'program': 'windows', 'scale': 2, 'filter': False, 'fast_effects': False, 'fullscreen': False, 'mute': False, 'two': False,
            'smooth': False, 'start': 0}


def load_settings():
    try:
        return {**DEFAULTS, **json.loads(SETTINGS.read_text(encoding='utf-8'))}
    except (OSError, ValueError):
        return dict(DEFAULTS)


def slots():
    """[(slot, label)] of the machine states in port/build/states (slot1..8.state)"""
    exe_time = EXE.stat().st_mtime if EXE.exists() else 0
    out = []
    for n in range(1, 9):
        st = STATES / f'slot{n}.state'
        if not st.exists():
            continue
        when = time.strftime('%Y-%m-%d %H:%M', time.localtime(st.stat().st_mtime))
        label = f'Slot {n}  (F{n + 4})  saved {when}'
        info = STATES / f'slot{n}.input.json'
        try:
            d = json.loads(info.read_text(encoding='utf-8'))
            label += f'  frame {d.get("frame")}'
            note = d.get('note', '')
            if 'make_test_states' in note:
                label += {1: '  -- first battle, start', 2: '  -- first battle, end (victory follows)', 3: '  -- second battle, deployment'}.get(n, '')
        except (OSError, ValueError):
            pass
        if exe_time and st.stat().st_mtime < exe_time:
            label += '   [older than the Windows program: may not load]'
        out.append((n, label))
    return out


class Launcher:
    def __init__(self, root):
        self.root = root
        root.title('Final Fantasy Tactics -- native build')
        root.resizable(False, False)
        s = load_settings()
        pad = {'padx': 10, 'pady': 4}
        frm = ttk.Frame(root, padding=12)
        frm.grid()

        ttk.Label(frm, text='Program', font=('Segoe UI', 10, 'bold')).grid(row=0, column=0, sticky='w')
        self.program = tk.StringVar(value=s['program'])
        win_ok = EXE.exists()
        ttk.Radiobutton(frm, text='Windows build (no Docker)' + ('' if win_ok else '  -- not built yet'), variable=self.program,
                        value='windows').grid(row=1, column=0, columnspan=2, sticky='w', **pad)
        ttk.Radiobutton(frm, text='Docker build (Docker Desktop must be running; slower)', variable=self.program,
                        value='docker').grid(row=2, column=0, columnspan=2, sticky='w', **pad)

        ttk.Label(frm, text='Picture', font=('Segoe UI', 10, 'bold')).grid(row=3, column=0, sticky='w', pady=(10, 0))
        row = ttk.Frame(frm)
        row.grid(row=4, column=0, columnspan=2, sticky='w', **pad)
        ttk.Label(row, text='Resolution:').pack(side='left')
        self.scale = tk.StringVar(value=f'{s["scale"]}x')
        ttk.Combobox(row, textvariable=self.scale, values=['1x', '2x', '3x', '4x'], width=5, state='readonly').pack(side='left', padx=6)
        ttk.Label(row, text='(the original is 1x)').pack(side='left')
        self.filter = tk.BooleanVar(value=s['filter'])
        ttk.Checkbutton(frm, text='Texture filter (smoother sprites and text at 2x-4x; T toggles it while playing)', variable=self.filter).grid(row=5, column=0, columnspan=2, sticky='w', **pad)
        self.smooth = tk.BooleanVar(value=s['smooth'])
        ttk.Checkbutton(frm, text='Smooth window scaling (bilinear)', variable=self.smooth).grid(row=6, column=0, columnspan=2, sticky='w', **pad)
        self.fullscreen = tk.BooleanVar(value=s['fullscreen'])
        ttk.Checkbutton(frm, text='Full screen (F11 toggles it while playing)', variable=self.fullscreen).grid(row=7, column=0, columnspan=2, sticky='w', **pad)

        ttk.Label(frm, text='Game', font=('Segoe UI', 10, 'bold')).grid(row=8, column=0, sticky='w', pady=(10, 0))
        self.fast = tk.BooleanVar(value=s['fast_effects'])
        ttk.Checkbutton(frm, text='Fast effects (ability animations at 60 fps; the original plays them at 15-30)', variable=self.fast).grid(row=9, column=0, columnspan=2, sticky='w', **pad)
        self.mute = tk.BooleanVar(value=s['mute'])
        ttk.Checkbutton(frm, text='No sound', variable=self.mute).grid(row=10, column=0, columnspan=2, sticky='w', **pad)
        self.two = tk.BooleanVar(value=s['two'])
        ttk.Checkbutton(frm, text='Two players on this PC (player 2 = the second gamepad)', variable=self.two).grid(row=11, column=0, columnspan=2, sticky='w', **pad)

        ttk.Label(frm, text='Start', font=('Segoe UI', 10, 'bold')).grid(row=12, column=0, sticky='w', pady=(10, 0))
        self.starts = [(0, 'Power on (title screen)')] + slots()
        self.start = tk.StringVar()
        labels = [l for _, l in self.starts]
        chosen = next((l for n, l in self.starts if n == s['start']), labels[0])
        self.start.set(chosen)
        ttk.Combobox(frm, textvariable=self.start, values=labels, width=78, state='readonly').grid(row=13, column=0, columnspan=2, sticky='w', **pad)

        buttons = ttk.Frame(frm)
        buttons.grid(row=14, column=0, columnspan=2, sticky='we', pady=(14, 0))
        ttk.Button(buttons, text='Play', command=self.play, width=14).pack(side='left', padx=4)
        ttk.Button(buttons, text='Rebuild the Windows program', command=self.rebuild).pack(side='left', padx=4)
        ttk.Button(buttons, text='Make test states', command=self.make_states).pack(side='left', padx=4)
        ttk.Button(buttons, text='Close', command=root.destroy).pack(side='right', padx=4)
        ttk.Label(frm, text='Keys: arrows, X = confirm, Z = cancel, A/S = square/triangle, Enter = start, F1-F4 save a state, F5-F8 load, '
                            'F9 scene, F12 screenshot, Tab fast forward, Esc quit', foreground='#555', wraplength=560).grid(row=15, column=0, columnspan=2, sticky='w', pady=(10, 0))

    def current(self):
        n = next((n for n, l in self.starts if l == self.start.get()), 0)
        return {'program': self.program.get(), 'scale': int(self.scale.get().rstrip('x')), 'filter': self.filter.get(), 'fast_effects': self.fast.get(),
                'fullscreen': self.fullscreen.get(), 'mute': self.mute.get(), 'two': self.two.get(), 'smooth': self.smooth.get(), 'start': n}

    def save(self, s):
        try:
            SETTINGS.write_text(json.dumps(s, indent=1), encoding='utf-8')
        except OSError:
            pass

    def play(self):
        s = self.current()
        self.save(s)
        if not VENV_PY.exists():
            messagebox.showerror('Missing', f'The viewer needs the Python environment {VENV_PY} (see HOW-TO-PLAY.md).')
            return
        if s['program'] == 'windows' and not EXE.exists():
            messagebox.showerror('Not built', 'The Windows program is not built yet: press "Rebuild the Windows program" (about an hour the first time).')
            return
        args = [str(VENV_PY), str(NATIVE / 'play_gl.py'), '--scale', str(s['scale'])]
        if s['program'] == 'windows':
            args.append('--native-exe')
        if s['filter']:
            args += ['--filter', 'epx']
        for key, flag in (('fast_effects', '--fast-effects'), ('fullscreen', '--fullscreen'), ('mute', '--mute'), ('two', '--two'), ('smooth', '--smooth')):
            if s[key]:
                args.append(flag)
        if s['start']:
            args += ['--load-slot', str(s['start'])]
        subprocess.Popen(args, cwd=str(PORT.parent), creationflags=NEW_CONSOLE)     # a console of its own: the game's messages, and errors if any

    def rebuild(self):
        if not messagebox.askyesno('Rebuild', 'Rebuild the Windows program now? (minutes after small changes, about an hour the first time; '
                                              'saved states of an older build will not load afterwards: "Make test states" remakes the test ones)'):
            return
        subprocess.Popen(['cmd', '/k', sys.executable.replace('pythonw', 'python'), str(NATIVE / 'win' / 'build_win.py')], cwd=str(PORT.parent), creationflags=NEW_CONSOLE)

    def make_states(self):
        if not messagebox.askyesno('Test states', 'Make the three test states (slots 1-3: first battle start / end, second battle deployment)? '
                                                  'This replaces slots 1-3 and takes a few minutes.'):
            return
        subprocess.Popen(['cmd', '/k', sys.executable.replace('pythonw', 'python'), str(PORT / 'tools' / 'make_test_states.py')], cwd=str(PORT.parent), creationflags=NEW_CONSOLE)


def main():
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == '__main__':
    main()
