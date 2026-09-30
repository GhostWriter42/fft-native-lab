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
| `.\port\native\lockstep.ps1 [-Frames N] [-Log N] [-Rebuild]` | **whole-program lockstep**: the native game and the original code boot side by side from `main()` on the same HLE, RAM compared at every VSync (445 frames identical up to the first overlay load) |
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
2. One native module per overlay with its own symbol map; boot -> title -> world -> battle with a null renderer.
3. A renderer, audio, then HD assets and netcode.
