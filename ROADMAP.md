# Roadmap: HD overhaul, then multiplayer

Built from what was measured overnight (see `PORT-FEASIBILITY.md`, `ARCHITECTURE.md`, `LANDSCAPE.md`). Effort figures are **my rough
estimates for one focused developer**, meant for comparing options, not for scheduling; they will move once the first phase of a
route is done.

## The two routes

| | **E — emulator-first HD** | **N — native port** |
|---|---|---|
| What you ship | a texture pack + patches for an emulator (DuckStation-style texture replacement, widescreen/geometry options) | a native engine that runs the game from the user's own disc |
| HD quality ceiling | limited to what the emulator can replace/upscale; 3D and UI resolution follow emulator options; sprites replaced texture-by-texture | full control: real HD renderer, float geometry, widescreen, high refresh, replaced sprites/UI/maps/audio |
| Multiplayer | not possible without patching the emulator | native lockstep/rollback netplay is designed in |
| Effort to first visible result | days | months (a renderer, GTE, audio, platform layer first) |
| Main risk | you hit the emulator's limits (widescreen breaks FFT's 3D maps per reports) | scope: it is a port, and the last 10% (audio, save, movies, edge cases) is long |

**Recommendation:** run **E as a cheap experiment** (it teaches what the art needs), and start **N only if you want the multiplayer
and full-control outcome** — because multiplayer is what needs N. The one thing that is *already* proven and independent of both:
the game's rules engine runs headless and deterministically (spike 3), so a *rules-only* multiplayer prototype (server-authoritative
battles with a simple front end) can start before any renderer exists.

## Route N, phase by phase

| Phase | Goal | Deliverable | Key risk / open question | Rough effort |
|---|---|---|---|---|
| **N0** | Groundwork (**done**) | portify transform, 97.8% files compile, RAM image, native attack, hazards list, asset decoders | — | done |
| **N1** | The platform shim | software GTE (12 commands) + libgte API (33 functions) — **done 2026-09-30**: `port\native\gte\`, 35 libgte functions match the original machine code in 128,200 differential trials; BIOS services (56, mostly trivial), heap/CD/card/pad stubs, `rand` reimplemented with **verified** constants (the documented LCG is in place but unverified against the BIOS ROM). Measured link boundary of `src/main`+`src/battle`: 201 externals (GPU ~50, SPU ~35, CD ~16, pad/events/timers ~20, libc 6, other-overlay entry points ~20, asm-only ~12) — see `PORT-FEASIBILITY.md` | GTE exactness vs hardware (only matters for fidelity; determinism only needs both peers to agree) | 2–4 weeks (GTE part done) |
| **N1b** | The oracle (**built**) | `port\native\r3000\`: an R3000 interpreter that runs the original machine code from the disc, so any native function can be diffed bit-for-bit against the original — no emulator needed. ~150k trials over ~100 battle formulas: 0 differences | only functions with a fully native call tree are covered so far; next: a generic fuzzer over all ~2,300 main+battle functions | done (extend as the port grows) |
| **N2** | Overlay modules + main loop | one native module per overlay (OPEN/WLDCORE/WORLD/BATTLE/EVENT/EFFECT) with its own symbol map, data-image reload on switch, the boot -> title -> world -> battle loop running with a null renderer. **Started 2026-09-30** (`NATIVE-RUNTIME.md`): PS1-address scheme (native functions reached through trampolines, RAM keeps canonical pointers), coroutine threads with the retail scheduling, MIPS-semantics division, native versions of the BATTLE hand-written routines, and a boot probe — the original code boots on the interpreter through the logos and system loads to the OPEN overlay with ~20 HLE'd SDK behaviours | remaining: per-overlay loading/trampolines, HLE of the rest of the SDK (CdRead2 streaming, pad, GPU lists), running the native build under the same HLE and comparing RAM with the original at every VSync | 3–6 weeks (first third done) |
| **N3** | A renderer | GPU ordering tables -> a hardware renderer (GT4/FT4/GT3/F3/F4/G4/SPRT/TILE/LINE, draw-mode/tpage, VRAM moves); float GTE path for geometry; 256x240 -> any resolution; widescreen with camera/culling changes | correctness of texture/CLUT/semi-transparency emulation; perspective/affine differences | 4–8 weeks |
| **N4** | Audio | SPU + Suzuki sound driver (SMD sequences, ADPCM samples), CD-XA/STR streams or converted assets | timing exactness of the driver; movie (STR/MDEC) playback or replacement | 3–6 weeks |
| **N5** | Input, save, boot from disc image | pad mapping, memory-card save format, `.bin/.cue` reader, options | low | 1–2 weeks |
| **N6** | HD assets | index-space sprite upscaling (138 sheets, palette swaps intact), UI/font replacement (tiny), map textures (format not yet decoded), effects, world map, optional audio remaster | map/effect formats; art volume; licensing of replacement art | open-ended; sprites alone 2–6 weeks with tooling |
| **N7** | Netcode | lockstep or rollback on the RAM-image snapshot; **split gameplay and cosmetic RNG**; host-seeded event RNG; determinism CI (state hash per frame) | desync sources: uninitialised locals (zero-init flag), VSync-driven RNG, cooperative-thread state | 3–6 weeks |
| **N8** | Co-op / PvP mode | design + UI: per-player unit control, lobby, host authority, reconnect (snapshot) — WotL's Rendezvous/Melee are the precedent | game design, not engineering, dominates; story events assume one party | 4–8 weeks |

Overlap is normal: N3/N4 can run in parallel with N2 once it boots, and N7 can be prototyped on the headless rules engine before N3.

## Route E, in order

1. Pick the emulator (DuckStation-class texture replacement; check current widescreen/PGXP state for FFT) — **needs a download, ask first**.
2. Play the rebuilt disc (`fft_decomp\build\disc\output-scus-94221.bin`, byte-identical to the original) to set a baseline.
3. Dump the game's textures, map them to sprite sheets/frames (`port\tools\sprites_all.py`, `shp_frames.py`), prepare replacements in
   index space so palette swaps keep working, and load them as a texture pack.
4. Widescreen/camera: the decomp lets you change the projection/culling in the game code itself and rebuild a *modified* disc — but the
   repo's tooling enforces byte-exactness, so a modding build path (skip the hash check, or patch data instead) is needed first.

## A rules-only multiplayer prototype (can start now)

`port\native\attack.ps1` already resolves a full weapon attack with the game's own code: 244 functions, real weapon data, no graphics.
Extending it to a duel loop (turn order via CT, HP application, KO) gives an authoritative battle core; a thin client could be a
terminal or a 2D front end. It would prove the netcode design (commands over the wire, host-seeded RNG, state hash) long before a
renderer exists, and its core carries straight into route N.

## Independent of the route

* **Upstream cleanup** — 3 patch series ready in `patches\` (apply-tested). Publishing is your call and needs your GitHub identity.
* **A modding build path** — the decomp only builds byte-exact discs. **Prototype done and proven** (`MODDING.md`, branch
  `mod-build-path`): with an opt-in flag, a changed function is patched into its original slot and the disc is rebuilt with valid
  sector checksums; the example mod changed exactly 2 bytes. Remaining: functions that must *grow* (code caves / relocation), and a
  data-patch layer (`module symbol+offset bytes`) for table edits.
* **Legal shape** — bring-your-own-disc, no assets in the repo/patches; a hosted public service with Square Enix content is riskier
  than a client (see `LANDSCAPE.md`).

## Decisions I need from you

1. Route: E first, N, or both?
2. OK to download an emulator (name/size will be stated first) for route E? (No longer needed for *validation* — the R3000 oracle covers that — only for trying route E itself.)
3. Publish the upstream branches (and under which identity), or keep them local?
4. Multiplayer scope for the prototype: PvP duel, co-op missions, or "decide later"?
