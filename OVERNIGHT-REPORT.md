# FFT decomp — working report

Started 2026-09-29 (evening). Last updated 2026-09-30 (late). Newest status at the top.

## Latest: the native game draws real frames, bit-identical with the original code

The decomp's C (plus a short list of reviewed patches for places where it relied on what the 1990s MIPS compiler happened to do) is compiled with a modern compiler into a
32-bit program that runs the **whole game** -- boot, title menu, new game, name entry, the opening events, the first battle, and (with a cheat that ends that battle) the world
map -- on a high-level emulation of the SDK's hardware layer and a software model of the GPU. Next to it runs the **original machine code from your disc** on a MIPS R3000
interpreter, on the same platform layer. The two machines are compared at every frame: **all 2 MiB of RAM, the scratchpad, every call into the platform layer, and (new) the
1 MiB of VRAM the two machines drew**. A difference is localised to a function, a call argument, or a single store.

What it shows now (pictures in `port\samples\native-frames\`, rendered by the native build at 2x for viewing): the title screen, the memory-card warning, the location banner
"Orbonne Monastery", the opening event's dialogue with sprites ("God, please help us sinful children of Ivalice."), the **world map** with its menu (Move / Formation /
Brave Story / Tutorial / Data / Option, War Funds, the party marker), and the **first battle**: textured 3D map, units, menus, the status panel with Ramza's portrait, rain.

| Verification | Result |
|---|---|
| title -> first battle, random play (40 seeds x 12,000 frames, 12-16 docker runs in parallel) | RAM and VRAM identical at every frame |
| world map, random play (24 seeds x 9,000 frames, poked start state) | RAM and VRAM identical at every frame |
| 60,000-frame games (40 seeds) | 35 of 40 identical at the last full run; the 5 others diverged only in dead stack garbage (now tolerated; re-run pending) |
| function coverage reached by these runs | main 325/821, battle 1,247/1,912, events 272/777, world 608/1,013, wldcore 138/435, opening 70/150 (effect overlays barely) |

What this took (all documented in `NATIVE-RUNTIME.md` section 9): whole-program lockstep with overlay switching (126 modules), CD streaming / event / pad / timing models,
function-level replay to find the first different call or argument, a native store watch, soak tooling with random-play seeds, a software GPU (VRAM, rasteriser, the libgpu
corner cases), native versions of the hand-assembled routines the game needs to draw (text blitters by hand, the four GTE map-polygon routines by `tools/mips2c.py`, a small
static binary translator), and **39 reviewed, exact-match patches (48 source sites)** for retail-ABI accidents that the comparison found (stale-register arguments, narrow return values, adjacent
locals used as arrays, `SetSemiTrans(&pointer)`, an out-of-bounds index, uninitialised locals, NULL-pointer reads ...). `git diff` of `fft_decomp` stays empty: patches apply to
the generated copy only, so the master-derived branches stay byte-exact with the disc.

What it means for your goals: the **HD renderer** has its foundation -- the game's GPU traffic goes through one model that can render at any internal scale (the picture
pipeline and the verification harness are in place: a scaled renderer can be checked against the 1x one); the **multiplayer** question gets its first hard evidence -- two
independent builds of the same game stay bit-identical over tens of thousands of frames of real play, which is what lockstep netcode needs.

Still open (details: `NATIVE-RUNTIME.md` section 10): audio (SPU / XA) and movies, the effect overlays (ability animations) beyond a handful of files, deeper world-map play,
checking the GPU model against a real emulator's frames (dithering and hardware edge rules are not modelled), then the HD renderer and a real window/input/audio layer.
Nothing was pushed, published or downloaded; everything is local commits in the project repo.

Reproduce (PowerShell, Docker running, `port\build\portable` built by `.\port\tools\mktree.ps1`):

    .\port\native\lockstep.ps1 -Frames 12000 -Scenario title -TitleToBattle -PadSeed 10 -Gpu -ShotEvery 1000 -ShotFrom 3000 -ShotScale 2     # pictures in port\build\shots
    .\port\native\soak.ps1 -From 1 -Count 40 -Frames 12000 -Parallel 12 -Gpu                                                               # the random-play soak
    .\port\native\lockstep.ps1 -Frames 2600 -Scenario title -TitleToBattle -PadSeed 5 -PadFrom 1200 -Gpu -ShotEvery 200 -ShotFrom 1200 -PokeWhen 'g_open_system_runtime_flags=0x41c0:g_open_system_result=1,g_open_system_runtime_flags=0x41c0:0x8005794c=1'   # the world map

## TL;DR

* The decomp is real: it builds a **byte-identical** disc from your BIN (re-verified after the final change).
* **172 speculative workarounds removed** from the repo across 6 verified commits (local branch, unpushed); plus a 2-commit Windows fix branch. Patch files are in `patches\`.
* The game's own combat code now **runs natively** against real disc data, and the whole portability picture is measured (`PORT-FEASIBILITY.md`). That is the strongest evidence so far that an HD / multiplayer-capable native build is feasible.
* **Modding works end to end** (prototype): edit a line of C -> rebuild the disc -> only that change differs (1 sector, 2 bytes for the example) — `MODDING.md`. Balance experiments can also be previewed natively in seconds, no emulator.
* Nothing has been pushed, published or opened anywhere. Decisions for you are at the bottom.

## Look at these first

| File | What it is |
|---|---|
| `MODDING.md` | how to edit game code and rebuild a playable disc (opt-in mod mode), its limits, and native preview of balance changes |
| `ROADMAP.md` | the two routes (emulator-first vs native), phase-by-phase plan with deliverables/risks/rough effort, a rules-only multiplayer prototype that can start now, and the four decisions needed from you |
| `PORT-FEASIBILITY.md` | measured portability, native spikes, art inventory, rendering facts, determinism hazards (RNG streams, BIOS surface, UB scan), rollback notes, recommended native build flags |
| `ARCHITECTURE.md` | boot sequence, frame timing (VSync advances the RNG!), the top-level overlay loop, battle loop, cooperative threads, and what they imply for a native port / lockstep netplay |
| `LANDSCAPE.md` | what exists around FFT (official remaster, WotL co-op/PvP precedent, widescreen patch, sprite tools) with sources |
| `port\HAZARDS.md` | 619 undefined-behaviour candidates from a `gcc -O2` scan of the portified sources (worklist for a native port) |
| `port\build\sprites\_contact.png` | all 138 unit sprite sheets decoded from your disc |
| `port\native\OUTPUT-ram.txt`, `OUTPUT-attack.txt` | the game's weapon-XA formula over the real weapon table; and a complete weapon attack (hit/evade/crit/zodiac/damage) resolved natively |
| `port\samples\` | decoded sprite sheet, index-space upscale demo (4 palettes), frames assembled from a `.SHP` |
| `patches\` | `git format-patch` series for both branches + how to apply |

## How to use what's here

| Thing | Where | Notes |
|---|---|---|
| Decomp repo (clone of `adamrt/fft_decomp`) | `fft_decomp\` | local branches only |
| Your disc image | `game\Final Fantasy Tactics.bin` (+ gitignored copy `fft_decomp\scus-94221.bin`) | hash-verified against the project's expected SHA-256 |
| Windows stand-in for `make` | `fft.ps1` | `validate`, `build disc`, `diff FUNC` (~2 s), `check-config`, `test`, `fmt`, `fmt-check`, `shell`; `FFT_REPO` env var points it at another checkout |
| Docker image | `fft-decomp-dev:local` (1.56 GB) | `.\fft.ps1 bootstrap` rebuilds; `docker rmi fft-decomp-dev:local` removes |
| Native-port tooling | `port\` | `probe.ps1` (portability probe), `hazards.ps1` (UB scan), `native\zodiac.ps1` + `native\ram.ps1` (native spikes), `tools\` (portify, sprite decoders, analyses). Nothing there touches `fft_decomp`. |
| Scratch worktree | `fft_sweep\` (now on branch `mod-build-path`) | used for the background sweeps, the docs branch and the modding build. **Its `build\disc\` holds the MODDED disc (zodiac "best" +100%); the pristine byte-identical rebuild is `fft_decomp\build\disc\`.** Remove with `git -C fft_decomp worktree remove ..\fft_sweep --force` (branches survive). |

## Baseline (verified before any change)

`check-config` OK (126 modules / 5,296 functions) · `validate` OK · `build disc` OK, rebuilt disc hash identical to the original · `test` OK · `fmt-check` OK.

## Branches (all local, all unpushed; upstream `origin/master` was still `cbf25cd` when last fetched)

| Branch | Commits on top of `master` (cbf25cd) |
|---|---|
| `windows-line-endings` | `.gitattributes` (`* text=auto eol=lf`) + README "Windows" note. Fixes the CRLF checkout Git for Windows' system `autocrlf=true` produces (verified with a fresh clone). |
| `remove-unneeded-pins-and-barriers` @ `caa3530` | 25312b2 −9 register pins · 0cbe001 −2 asm barriers · 1194b66 −11 volatile casts (game) · 41cef43 −132 in the PS1 SDK (122 barriers, 8 pins, 2 casts) · 0fd89ff −9 volatile pointer views (game) · caa3530 −9 more in the SDK (7 pins, 2 barriers). Every commit verified (`validate` + `fmt-check` + `check-config`); the **final state also with a full `build disc` — disc identical**. |
| `document-native-port-ub` (from `master`, in worktree `fft_sweep`) | 4a1956b `QUIRKS.md`: undefined behaviour a native build must neutralise (docs only; each entry read by hand). |
| `mod-build-path` (from `master`, in worktree `fft_sweep`, currently checked out there) | 9fc2235 opt-in modding build (`TOOLS_MOD_MODE`) · 1a92a84 example mod (zodiac best +100%). **Downstream only.** Proven end to end: the modded disc differs from the original in 1 sector / 2 bytes (`MODDING.md`). |
| `sweep-scratch` (worktree) | scratch, not for publishing |

Commits are authored as your global placeholder identity `Test User <test@example.com>`; re-author before anything goes upstream.

## Sweep results (method: change one thing; `tools diff FUNC` must still byte-match; keep only if it does)

| Sweep | Round 1 tried → removed | Round 2 tried → removed |
|---|---|---|
| register pins, game code | 368 → 9 | 359 → 0 |
| empty asm barriers, game code | 342 → 2 | 331 → 0 |
| volatile reload casts/views, game code | 30 → 11 | 39 → 9 (matcher widened to `T* volatile*`) |
| register pins, SDK (`src/psyq`) | 326 → 8 | 318 → 7 |
| empty asm barriers/ties, SDK | 346 → 122 | 224 → 2 |
| volatile casts, SDK | 5 → 2 | 3 → 0 |
| **total** | | **24 pins + 126 barriers + 22 volatile = 172**; a cascade check on the touched files found nothing further |

The maintainer's "unpinned, GCC does X" comments are accurate for game code; the SDK reconstructions had the speculative ones.
Dropped after investigation: `goto` removal (the remaining gotos are documented as deliberate — for/while/do engage GCC's loop optimizer), bulk cast removal (3,800 casts, huge unreviewable diff), retyping "never returns a value" functions to `void` (205 game candidates, but 197 are EFFECT helpers dispatched through pointers whose callers compare the result), duplicate local types (none left).

## Native-port groundwork (details in `PORT-FEASIBILITY.md`)

* Build-time transform (`port\tools\portify.py`, repo untouched): **5,166 / 5,284 (97.8%)** files compile with `gcc -m32`; the remaining 118 need real GTE/CPU-ABI asm (70 game files need ~10 GTE macros; the rest is SDK). The transform itself had a semantic bug (dropped asm that *assigns*), found via the UB scan and fixed.
* **Native spikes**: the game's own zodiac-compatibility code and weapon-XA formula run natively against **real data from your disc** through a PS1 RAM image mapped at `0x80000000` (symbols linked at their original addresses) — a workable memory-map strategy for a 32-bit build; the 295 files with hardcoded `0x80xxxxxx` addresses need no rewrite.
* **A complete weapon attack resolves headlessly**: 244 game functions (the whole combat-formula layer and its dependencies, 0 stubs) link natively and reproduce FFT's mechanics — 85% hit from the front vs 100% from side/back (class evade), ≈4% criticals, zodiac bonus, damage `XA×WP` — `port\native\attack.ps1`, output in `port\native\OUTPUT-attack.txt`. This is the deterministic core an authoritative co-op/PvP host needs.
* **Determinism (matters for lockstep netplay)**: `rand()` is a BIOS routine (must be reimplemented), one RNG stream serves both gameplay and cosmetics (particles, weather) → split them; the event-script RNG is seeded from `VSync`; 56 BIOS services form the platform surface; 619 UB candidates catalogued; recommended flags `-m32 -fno-strict-aliasing -fwrapv -funsigned-char -fcommon -ftrivial-auto-var-init=zero`.
* **Rollback**: with a RAM-image design a save state is a 2 MiB memcpy; caveats (cooperative thread stacks, VSync-driven timing) are written down.
* **Art scoping and pipeline**: 138 unit sprite sheets (~24x40 frames, 16 palettes) all decode cleanly; the `.SHP` frame tables are decoded (208 frames per set, verified visually; JSON export of every frame's part rectangles); an **index-space upscale keeps the game's palette swaps working** (`port\samples\upscale_knight_index_space.png`). UI is tiny; video is 65% of the disc. Rendering needs only a small GPU primitive set at 256x240.

## Decisions for you

1. **Contribute upstream?** The branches are ready; publishing means a GitHub fork + PR/issue under your identity (I will not do that without you). Suggested order: `windows-line-endings`, then the pin/barrier branch. Fix the commit identity first.
2. **Direction:** emulator-first HD (texture replacement/upscaling; fast, no port; needs an emulator download) vs. native port (slow, full control, needed to own the netcode). The spikes show the native route is feasible; `LANDSCAPE.md` covers what exists.
3. **Multiplayer scope:** co-op / PvP battles first (deterministic lockstep on the game's own RNG, split gameplay/cosmetic streams); MMO-style is a separate product.
4. **Next native milestone** if you choose the native route: a software GTE + libgte API so the last 70 game files compile, then a first module link (battle formulas) with an emulator-trace oracle.

## Log

- 2026-09-29 evening: baseline verified; `.gitattributes` + README fix; pin/barrier/volatile sweeps and commits; portability probe, portify, native spikes 1–2; sprite decode; landscape research.
- 2026-09-30 after midnight: round-two sweeps (worktree) verified and cherry-picked; final full verification of the main branch (disc identical); patches exported; UB/hazard scan; RNG/BIOS/rollback analysis; transform bug fixed.

---

## Late-night update: native-port verification

**New: an oracle that needs no emulator.** `port/native/r3000/` is a small MIPS R3000 interpreter that runs the *original*
machine code from your disc image. A native function can now be run on the same inputs both ways and compared bit for bit
(return value + every data byte of the 2 MiB RAM image). This replaces the "needs an emulator download" item in ROADMAP
(an emulator is still optional for trying route E itself).

| What | Trials | Result |
|---|---|---|
| Native libgte (35 functions) vs original code (`port/native/diff_libgte.ps1`) | 128,200 | **0 differences** (only the scratch GTE register VZ0 differs after MulMatrix*) |
| ~100 battle-formula handlers vs original code (`port/native/diff_formulas.ps1 -Trials 1500`) | 149,086 | **0 differences, 0 native crashes** |
| Native C replacements for the 8 hand-assembled BATTLE routines (`port/native/diff_asm.ps1`) | 28,000 | **0 differences** |
| GTE unit tests (`port/native/gte/tests/run.ps1`), incl. hardware-style perspective divide vs exact division | 4.58 M checks | all pass |

What the fuzz surfaced (all now understood; details in `PORT-FEASIBILITY.md`, "The oracle"):
1. **Formula 37** looked like a real native-vs-retail divergence (1 random state in ~1,440). A replay mode (`diff_formulas.ps1 -Replay '37,1332'`,
   call trace + RAM hash at every call on both machines) traced it to a harness artifact: random input made the original copy 14 bytes of
   *machine code* as if they were table data, and in the native process those bytes are my x86 trampolines. Such trials are now detected and skipped.
2. **Divide by zero** (formula 66, 3 of ~150k random states): x86 traps where MIPS returns a defined value. Real data never divides by zero here,
   but mods could; the port needs a defined behaviour (source-level checked division), which multiplayer also needs so peers agree.
3. Function pointers in game data hold PS1 addresses; x86 `jmp` trampolines at those addresses fix that (proven in the harness).
4. `gen_symbols.py` was wrongly linking 35 overlay functions at their PS1 addresses; fixed.

**Link boundary** of `src/main` + `src/battle` (`port/native/boundary.ps1`): 2,308 of 2,316 files compile natively (the 8 failures are the
hand-assembled routines, now replaced in `port/native/replacements/`); only **201 externals** remain, and they are exactly the platform layer:
GPU/primitives ~50, SPU ~35, CD ~16, pad/events/timers ~20, libc 6, other-overlay entry points ~20, asm-only ~12.

Docs updated: `PORT-FEASIBILITY.md` (new "oracle" section), `ROADMAP.md` (N1 done, new N1b), `port/README.md`.
Everything is committed in a *local* git repo at the project root (created tonight; `.gitignore` excludes the disc and the two clones).
Nothing was pushed or published.

Next candidates: a generic fuzzer over all ~2,300 main+battle functions (generic caller + the oracle), a divide-by-zero policy in `portify`,
the WORLD overlay in the oracle (its asm twins are written, not yet diffed), and the SDK shim (GPU primitives first, since that is where an HD renderer plugs in).
