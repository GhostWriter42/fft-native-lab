# FFT native lab

Experiments around [adamrt/fft_decomp](https://github.com/adamrt/fft_decomp), the byte-exact decompilation of Final Fantasy Tactics (PS1, US, SCUS-94221).
The goal is an HD build and, later, multiplayer. Everything here is work in progress.

What exists (details in `NATIVE-RUNTIME.md` and `OVERNIGHT-REPORT.md`):

* **A native build of the game** (`port/`): the decomp's C compiled with a modern compiler, running on a thin platform layer (software GPU, software sound chip, CD reader) instead of a console.
* **A differential oracle**: a small MIPS R3000 interpreter runs the *original* machine code from your disc next to the native build, and RAM, scratchpad, SDK calls and VRAM are compared at every frame
  (hundreds of random-play seeds, up to 60,000 frames each, identical). It is how the retail quirks that a modern compiler breaks were found.
* **A playable window** with save states, an HD prototype (2x-4x with EPX texture filtering), sound, and a two-player lockstep netplay prototype.
* Notes on modding the decomp and on the retail code (`MODDING.md`, `ARCHITECTURE.md`, `PORT-FEASIBILITY.md`, `ROADMAP.md`).

How to try it: `HOW-TO-PLAY.md` (Windows, Docker Desktop, Python with Pillow and tkinter, **your own disc image**).

## What is not here

No game data of any kind: no disc image, no extracted files, no screenshots or art made from the game, no save states. You need your own legally obtained copy of the game.
Two kinds of files are built on your machine from your own disc: the extracted game files, and `port/native/replacements/battle_asm3.c`
(a mechanical translation of a few hand-assembled routines; `python port/native/regen_asm3.py`).

This project is not affiliated with or endorsed by Square Enix. Final Fantasy Tactics is their trademark.

## Licence

The code and documents in this repository are MIT licensed (see `LICENSE`). The licence covers our own work only: it does not grant any rights in the game, its code, art or data,
nor in the decomp this builds on, which has its own owner and terms.
