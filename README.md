# FFT native lab

Experiments around [adamrt/fft_decomp](https://github.com/adamrt/fft_decomp), the byte-exact decompilation of Final Fantasy Tactics (PS1, US, SCUS-94221).
The goal is an HD build and, later, multiplayer. Work in progress; nothing here contains game data (see "What is not here").

## Where this stands (updated 2026-10-09)

| Area | State |
|---|---|
| Native build | The decomp's C compiles with a modern compiler and runs the whole game from the title screen through the first battle, the world map and the menus, on a small platform layer (software GPU, software sound chip, CD reader). |
| Proof of correctness | A MIPS R3000 interpreter runs the **original machine code** from your disc next to the native build; RAM, scratchpad, every SDK call and VRAM are compared at every frame. Hundreds of random-play runs of 12,000-60,000 frames are identical (latest: 32 random-battle seeds x 30,000 frames on the current upstream base). |
| Playable window | `play.ps1` (Python/Tk, slow: the CPU draws) and `play.ps1 -Gl`: **the graphics card draws** (OpenGL), at 1x-4x the original resolution, keyboard or gamepad, sound, save states, fast-forward, optional EPX texture filter (`-Filter`, T). **The game saves and loads** on a virtual memory card (`port/build/states/memcard0.mcr`, a standard `.mcr` image). Diagnostics: `-Record` (frame ring buffer, flashing detector), `-Compare` (live GPU vs software picture), the controller input of every session is logged and replayable |
| GPU renderer | The game's GPU command trace is replayed on the GPU; checked pixel by pixel against the software GPU on ~54,000 frames (title, dialogue, battle map, world map): 100% of frames within tolerance, 0.5-2 ms per frame. See `NATIVE-RUNTIME.md` "Result 9". |
| Sound | The game's sound driver runs; a software SPU (ADPCM, ADSR, reverb, noise generator, pitch modulation, 4-point cubic resampling across ADPCM blocks) plays through Windows audio, paced by the audio clock. libspu's own bookkeeping (the SPU heap, reverb attributes, key-on mask) is kept as the real library keeps it. The game streams no CD-XA audio |
| Two players | Deterministic lockstep over TCP (only controllers are sent), hot-seat and AI-ally control; in the GPU viewer too (`-Gl -HostPort` / `-Join`, `-Gl -Two` with two gamepads on one computer; tested: 3,000 frames, 49 state-hash checks equal). Joining a game in progress needs the CPU viewer |
| HD | 2x-4x rendering on the GPU with an optional EPX texture filter; replacement art is the next step. Widescreen needs game-side changes (projection, culling, 2-D layout), not a renderer option |

Details and evidence: `NATIVE-RUNTIME.md` (design + results), `OVERNIGHT-REPORT.md` (newest first), `port/README.md` (every script), `HOW-TO-PLAY.md`.

**Known issues / next** (2026-10-09)

* **Scenes**: recorded gameplay replays as a regression test (`port/tools/scene.py`; F9 in the GPU viewer). Two scenes from the owner's first session (power-on to the end of the first battle; victory, story scene and saving) are identical to the original code at every frame (`NATIVE-RUNTIME.md` Result 15).
* **Platform-layer audit**: `port/tools/hle_audit.py` lists the state each replaced SDK call must keep. libgpu (Result 10) and libspu (Result 13) are fixed and the memory card is new (Result 14); the GTE's flag bits are still to be reviewed.
* **Effect overlays**: 110 of the 512 effect files are code (the rest are scripts the battle code interprets). A test that forces abilities onto code-type effects crashes natively in the effect-script reader; not yet known whether the original misbehaves the same way there (Result 16).
* **A Windows build without Docker** works (`play.ps1 -Native`, `WINDOWS-NATIVE.md`): the owner's whole first session is identical to the original code at every frame; the CPU viewer and joining a network game in progress still use the container.
* **Fixed 2026-10-10: the first battle never ended in the play builds.** They were built with a test shortcut of the soak harness (`-Scenario title`) that skips `main_boot_reset_game_state`, which fills the KO status tables, so dead enemies never counted as defeated. Replaying the owner's session (auto-battle included) now reaches the victory scene. HD seams along mesh edges (texture lookups straying onto neighbouring texels at 2x-4x) are fixed too.

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
