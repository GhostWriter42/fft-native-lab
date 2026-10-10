# port/ — groundwork for a native (HD / multiplayer-capable) build, plus asset tools

Nothing here modifies `fft_decomp`. Generated output goes to `port\build\` (about 120 MB; safe to delete, everything regenerates).
Numbers and design notes: `..\PORT-FEASIBILITY.md`; plan: `..\ROADMAP.md`. Needs Docker Desktop running (image `fft-decomp-dev:local`)
and host Python 3 (Pillow for the image tools).

## Run these

| Command | What it does |
|---|---|
| `.\port\probe.ps1 [-Baseline]` | snapshot HEAD of `fft_decomp`, run `portify.py`, compile every file with `gcc -m32`, print pass rate + error histogram (**must run first**: it creates `build\portable`) |
| `.\port\hazards.ps1` | same tree with `-O2` + undefined-behaviour warnings -> `HAZARDS.md` |
| `.\port\native\zodiac.ps1` | native spike 1: the zodiac-compatibility formula over all sign pairings |
| `.\port\native\ram.ps1` | native spike 2: PS1 RAM image at `0x80000000`; weapon XA for every weapon in the real item table |
| `.\port\native\attack.ps1` | native spike 3: a full weapon attack (evade/hit/crit/zodiac/damage) with 244 game functions, 0 stubs |
| `.\port\native\recon.ps1 -Sources …` | which external functions does a set of sources still need? |
| `.\port\native\closure.ps1 -Sources … [-Harness x.c]` | grow a slice until it links (auto-adds sources of unresolved functions), optionally link + run a harness |
| `.\port\native\diff_libgte.ps1` | **differential test**: the native libgte (software GTE) vs the ORIGINAL libgte machine code on the R3000 interpreter (128,200 trials) |
| `.\port\native\diff_formulas.ps1 [-Trials N] [-Replay 'id,trial']` | differential fuzz of ~100 battle formulas, native vs original code (149k trials); `-Replay` traces one trial on both machines and shows where they first diverge |
| `.\port\native\diff_asm.ps1` | native C replacements for the hand-assembled BATTLE routines vs the original code (28,000 trials) |
| `.\port\native\boundary.ps1` | compile all `src/main` + `src/battle` natively (parallel) and list the 201 unresolved externals (the platform layer still to write) |
| `.\port\native\fuzz.ps1 [-Trials N] [-AutoInit] [-DivFix] [-Sdk] [-Replay 'index,trial;…']` | generic function-level differential fuzz: every simple-signature function of `src/main` + `src/battle` (2,314), native vs the original machine code; `-Replay` localises a divergence (call trace + RAM hashes on both machines) |
| `.\port\native\boot.ps1 [-Frames N] [-Log N]` | boot probe: the ORIGINAL game from `__SN_ENTRY_POINT` on the interpreter with the SDK hardware layer HLE'd; logs the SDK calls and CD reads |
| `.\port\native\lockstep.ps1 [-Frames N] [-Scenario title] [-TitleToBattle] [-PadSeed N] [-PokeWhen ...] [-Replay N] [-Watch ...] [-Gpu -Shot ...]` | **whole-program lockstep**: the native game and the original code boot side by side from `main()` on the same HLE, RAM compared at every VSync. Runs through the title menu, new game, the first battle (BATTLE + EVENT overlays) and the world map (WLDCORE + WORLD). Flags: `-TitleToBattle` (fixed START/CIRCLE presses into the first battle), `-PadSeed N -PadFrom F` (random play), `-PokeWhen 'sym=val:sym=val,...'` (cheats that steer into states input rarely reaches, e.g. the world map), `-Replay N` (function-level comparison of frame N: first different call / argument), `-Watch` (variables per frame; with `-Replay` compared at every function entry, plus the original's writers of the first one), `-Dump/-DumpAround` (calls with callers), `-NatWatch` (every store of the native game to a word: page protection + single step), `-Where N`, `-RunOnly/-BuildOnly`, `-Gpu -Shot 'f1,f2' -ShotEvery N -ShotScale k` (software GPU: VRAM compared at every frame, PNG screenshots to `build\shots`) |
| `.\port\native\soak.ps1 [-From 1] [-Count 40] [-Frames 12000] [-Parallel 12] [-PadFrom 1180] [-PokeWhen ...] [-NoBuild]` | **random-play soak**: one docker run per random controller seed (`padgen.ps1`), all in parallel, each a whole-program lockstep; summary, function-coverage union and NULL-page accesses in `build\soak\` |
| `.\port\native\lockstep.ps1 ... -NativeOnly -Hd 2..4 -SnapSave 'F:name' -SnapLoad name -DetCheck 'F:M' -HashEvery N` | the same harness, more modes: **`-NativeOnly`** runs the native game alone (no original, no comparison, ~260 frames/s); **`-Hd`** also renders the display buffers at 2x-4x (PNG twins `h*.png`); **`-SnapSave` / `-SnapLoad`** write / read the complete machine state (`build\states\name.state`, same program build only); **`-DetCheck`** = determinism self-test (save, run M frames recording state hashes, load, replay: equal); **`-HashEvery`** prints a game-state hash (two runs with the same inputs print the same lines); **`-Seats 2 -Seat2Units M -Pad2Seed N -Hotseat M`** two controllers routed by turn ownership (co-op groundwork); **`-CfgFile`** runs a ready-made run.cfg. More scenario lines for a run.cfg (`soak.ps1 -ExtraCfg 'a;b'`, `{seed}` substituted): `caster MASK SEED`, `autobattle MASK`, `hotseat MASK`, `effectmap SEED`, `traceabil 1`, `cdtrace 1`, `snapload` / `snapsave` -- see `NATIVE-RUNTIME.md` "Result 6" |
| `.\port\native\play.ps1 [-Hd 2..4] [-WorldCheat] [-Verify] [-Build]` | **play the native build in a window** (Docker + Python with Pillow/tkinter): keyboard pad, P pause, Tab fast-forward, F12 screenshot, **F1-F4 save / F5-F8 load the whole machine state**; `-Verify` runs the original code alongside and compares every frame. No sound yet. `play_test.py` runs the same protocol headless |
| `python port\tools\statediff.py A.state B.state --symbols port\build\native\ls\symbols_pc.ld` | where two saved machine states differ (per region, runs of words, RAM addresses resolved to symbols) |
| `play.py` sound (`--mute` to switch off) / `lockstep.ps1` run.cfg `audio 1`, `audiodump /shots/x.raw` + `python port\tools\wavstat.py x.raw out.wav` | **sound**: the game's music driver runs (its 240 Hz event handler is now fired on both machines of the lockstep), a software SPU (`hle\spu.c`: ADPCM voices, ADSR, volumes; reverb from the game's own preset tables) mixes it, `play.py` plays it through Windows waveOut (`native\audio_selftest.py` checks the device with silence). `wavstat.py` / `wavzcr.py` measure raw dumps |
| `port\build\venv\Scripts\python.exe port\native\gl_verify.py [--frames N] [--pad-seed S] [--random-battle] [--world-cheat] [--vram-check --fb-check] [--save F,F]` | **GPU renderer check**: the game runs with `gltrace 2` (its GPU command trace + the software picture of each frame), `gl_renderer.py` replays the trace with OpenGL at 1x on the graphics card and the two pictures are compared pixel by pixel; the worst frames go to `build\shots` (software \| GPU \| difference). `gl_selftest.py` checks the OpenGL context. Needs the venv (`python -m venv port\build\venv`, then `pip install moderngl glfw numpy pillow`) and the program built with `lockstep.ps1 -BuildOnly -Scenario title -Gpu` |
| `python port\tools\scene.py list / run NAME [--native] / accept NAME / from-input NAME FILE START END` | **scenes**: recorded gameplay (controller input from power-on, the memory card it started with, picture hashes at checkpoints; `port/scenes/*.json`) replayed in lockstep against the original code and compared with the accepted pictures; record new ones with F9 in the GPU viewer |
| `python port\tools\hle_audit.py [--all]` | **platform-layer audit**: for every SDK function the platform layer replaces, the globals and output parameters the real library code writes that code still running natively reads (`port/build/hle_audit.txt`) |
| `python port\tools\statepeek.py A.state 0x801908cc --array 21 0x1c0 0x00:1 0x05:1` | read memory of a saved state: hex dump, an array of records with chosen fields, or a word by symbol name |
| `python port\native\play.py --host 7777` / `--invite 7777` / `--join HOST:7777` | **two-player netplay** (prototype): two windows, each with its own native game; only the controllers (input delay 6 frames) and a state hash every 60 frames cross the TCP connection (`netplay.py`). `--host` waits for the guest and starts together; `--invite` starts playing alone and takes the guest into the game in progress (the host's whole-machine state is sent over the same link). Player 1 = controller 1, player 2 plays the turns of the units in `--seat2` (the AI allies of the first battle are made player-controlled by `--hotseat`). Save / load states are disabled while playing; `--test-frames N` runs the real viewer code headless (window hidden) for N frames |
| `python port\native\netplay_test.py [--frames N] [--delay D] [--jitter-ms J] [--hotseat M --seat2 M] [--dump-cfg FILE]` | headless two-player lockstep test: two containers, loopback socket, scripted random controllers for both players, equal state hashes required; `--dump-cfg` writes a run.cfg that replays the same controller values in the verified lockstep (`lockstep.ps1 -CfgFile`) |
| `.\port\native\gte\tests\run.ps1` | unit tests of the software GTE (4.58 M checks) |

## Tools (`tools\`)

| Tool | Purpose |
|---|---|
| `portify.py` | build-time transform: strips MIPS register pins and empty asm barriers, turns *assigning* asm ties into assignments, macro-izes conflicting volatile views, reports real asm |
| `hazards.py`, `closure.py`, `memmap.py`, `voidable.py` | analyses (UB warnings, name-based call closure, RAM layout by symbol prefix, never-returns-a-value functions) |
| `spr2png.py`, `sprites_all.py` | decode unit sprite sheets (`.SPR`) to PNG; all 138 + a contact sheet |
| `shp_frames.py` | assemble frames from a sheet using a `.SHP` frame table; `--json` dumps every frame's part rectangles |
| `sprite_upscale_demo.py` | index-space Scale2x: upscaling that keeps the game's palette swaps working |

`native\r3000\` is the MIPS R3000 interpreter that runs the original machine code (the oracle); `native\gte\` the software GTE + native libgte;
`native\replacements\` C versions of hand-assembled routines; `native\gen_funcs.py` / `gen_stubs.py` generate the address tables used by the oracle and
the x86 trampolines.

`native\` also holds the harnesses (`harness_*.c`, `lockstep.c`), the container build scripts, `gen_symbols.py` (linker-script symbols from `target\*.yaml`),
`gen_hle.py` (the SDK functions the HLE takes over + the overlay disc positions), `hle\` (the shared HLE of the SDK hardware layer: VSync, CD, SPU
transfers, callbacks), `bios_rt.c` (native BIOS services: rand, memset, strcpy, ...), `decode_handlers.py` (function-pointer table -> native table) and
the saved outputs (`OUTPUT-*.txt`). `NATIVE-RUNTIME.md` (project root) describes the design.
`samples\` has three decoded/derived images from your disc (local only).

## Next milestones (see ROADMAP.md)

1. ~~Software GTE + libgte API~~ (done, verified against the original code); the SDK/BIOS shim for the remaining 201 externals (`boundary.ps1`).
2. ~~One native module per overlay with its own symbol map; boot -> title -> world -> battle with a null renderer.~~ (done: 126 overlay modules; lockstep through title, new game, battle, events and the world map)
3. A renderer (`native\hle\gpu.c`: a software GPU model with VRAM; first PNG frames), audio, then HD assets and netcode.
