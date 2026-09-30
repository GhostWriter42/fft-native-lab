# FFT decomp — working report

Started 2026-09-29 (evening). Last updated 2026-09-30 ~01:00. Newest status at the top.

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
