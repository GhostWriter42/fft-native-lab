# FFT native lab

Experiments around [adamrt/fft_decomp](https://github.com/adamrt/fft_decomp), the byte-exact decompilation of Final Fantasy Tactics (PS1, US, SCUS-94221).
The goal is an HD build and, later, multiplayer. Work in progress; nothing here contains game data (see "What is not here").

## Where this stands (updated 2026-10-03)

| Area | State |
|---|---|
| Native build | The decomp's C compiles with a modern compiler and runs the whole game from the title screen through the first battle, the world map and the menus, on a small platform layer (software GPU, software sound chip, CD reader). |
| Proof of correctness | A MIPS R3000 interpreter runs the **original machine code** from your disc next to the native build; RAM, scratchpad, every SDK call and VRAM are compared at every frame. Hundreds of random-play runs of 12,000-60,000 frames are identical (latest: 32 random-battle seeds x 30,000 frames on the current upstream base). |
| Playable window | `play.ps1` (Python/Tk, slow: the CPU draws) and `play.ps1 -Gl`: **the graphics card draws** (OpenGL), at 1x-4x the original resolution, keyboard + sound + save states + fast-forward. Title screen, New Game (name and birthday entry), the opening scene and the first battle are playable; diagnostics: `-Record` (frame ring buffer, flashing detector), `-Compare` (live GPU vs software picture), controller input of every session is logged and replayable. |
| GPU renderer | The game's GPU command trace is replayed on the GPU; checked pixel by pixel against the software GPU on ~54,000 frames (title, dialogue, battle map, world map): 100% of frames within tolerance, 0.5-2 ms per frame. See `NATIVE-RUNTIME.md` "Result 9". |
| Sound | The game's sound driver runs; a software SPU (ADPCM, ADSR, reverb, 4-point cubic resampling across ADPCM blocks) plays through Windows audio, paced by the audio clock (no dropped chunks). Heard by the owner on 2026-10-03: "sounds normal" after the resampling and buffering fixes. Noise generator, pitch modulation and XA streams are not modelled. |
| Two players | Deterministic lockstep over TCP (only controllers are sent), late join, hot-seat and AI-ally control. Tested headless; not yet with the GPU viewer. |
| HD | 2x-4x rendering on the GPU; texture filtering and replacement art are the next steps. |

Details and evidence: `NATIVE-RUNTIME.md` (design + results), `OVERNIGHT-REPORT.md` (newest first), `port/README.md` (every script), `HOW-TO-PLAY.md`.

**Known issues / next** (2026-10-03)

* **Fixed 2026-10-03:** the opening scene's terrain rendered red because the software GTE did not implement its colour / lighting commands (NCS, NCT, ...); they are implemented and unit-tested now (`NATIVE-RUNTIME.md` "Result 11"). To re-check: the battle maps' old green / magenta tint, and further GTE hardware details (flag bits, the MVMVA far-colour quirk).
* **Platform-layer fidelity audit**: the flashing, text-less name-entry screen was caused by two SDK calls that the layer replaces (`PutDrawEnv`, `ResetGraph`) not keeping library state that the game's own code reads later; both are fixed (`NATIVE-RUNTIME.md` "Result 10"). The same class of bug may hide in other replaced calls, and the lockstep tests cannot see it because both machines share the layer: every replaced call will be audited against the real library code.
* **Scenes**: recording gameplay from save states as named, replayable scenes (state + controller log; the viewer already logs and replays controller input) to regression-test screens the random-play soaks never reach.
* The viewer has no two-player hooks yet, no texture filtering / widescreen on the GPU path, and sound lacks the noise generator, pitch modulation and CD-XA.

## How this differs from the repository it was forked from

The decompilation itself is **upstream's**. We do not change what it builds: everything that is not a documentation or build-hygiene change stays in this repository, outside the decomp.

* **The decomp fork** ([GhostWriter42/fft_decomp](https://github.com/GhostWriter42/fft_decomp); its default branch `fork` is upstream's master plus an "about this fork" notice, its `master` is a pure mirror) carries only three small branches on top of upstream's master, all byte-exact (`validate` 5,296 of 5,296 functions, `fmt-check` and `check-config` pass):
  * `remove-unneeded-pins-and-barriers` -- 172 compiler-workaround constructs (register pins, empty asm barriers, volatile views and casts, mostly in the reconstructed PS1 SDK) that no longer affect the output;
  * `windows-line-endings` -- `.gitattributes` and a README note so a Windows checkout does not break `check-config`;
  * `document-native-port-ub` -- `QUIRKS.md` only: undefined behaviour that a native build must neutralise (including a null test after a dereference that modern compilers delete).
  
  None of them has been proposed upstream yet. The upstream maintainer has independently fixed many of the same retail-ABI problems we ran into (arguments that retail passes through leftover registers, stack buffers that overflow into their neighbour); where they did, we dropped our version.
* **This repository** adds everything needed to run the decomp as a native program and to prove it equal to the original: `port/` (the platform layer `hle/`, the R3000 oracle `r3000/`, the software GTE, the lockstep / soak / fuzz harnesses, the GPU renderer, the viewers, netplay) plus documents. It never edits the decomp's files.
* **What is changed at build time, not in the source**: the build exports the decomp from git, strips the PS1 compiler pins (`port/tools/portify.py`) and applies **34 reviewed text patches** to that generated copy (`port/native/native_patches.py`) -- each is a place where the retail code relies on something the MIPS compiler happened to do (a value left in a register, a buffer that overflows into its neighbour, a read through a null pointer); the why of every patch is next to it. Native builds use `-fno-strict-aliasing -fno-aggressive-loop-optimizations -fwrapv -fno-delete-null-pointer-checks`.
* **Hand-assembled routines** (the text blitters, the GTE map queuers, thread switching, ...) have native C versions in `port/native/replacements/`, each checked against the retail machine code.
* **Not done on purpose**: no gameplay changes, no modding of the decomp (an opt-in modding build exists only as an unpublished branch), no changes to upstream's tooling or hashes.

## Try it

Windows, Docker Desktop, Python 3, **your own disc image** in `game/`; see `HOW-TO-PLAY.md`. The GPU viewer also needs `python -m venv port\build\venv` and `port\build\venv\Scripts\pip install moderngl glfw numpy pillow`:

    powershell -File port\native\play.ps1 -Gl -Hd 2          # the graphics card draws, 2x internal resolution
    powershell -File port\native\play.ps1                    # the CPU path (Tk window)

## What is not here

No game data of any kind: no disc image, no extracted files, no screenshots or art made from the game, no save states. You need your own legally obtained copy of the game.
Two kinds of files are built on your machine from your own disc: the extracted game files, and `port/native/replacements/battle_asm3.c`
(a mechanical translation of a few hand-assembled routines; `python port/native/regen_asm3.py`).

This project is not affiliated with or endorsed by Square Enix. Final Fantasy Tactics is their trademark.

## Licence

The code and documents in this repository are MIT licensed (see `LICENSE`). The licence covers our own work only: it does not grant any rights in the game, its code, art or data,
nor in the decomp this builds on, which has its own owner and terms.
