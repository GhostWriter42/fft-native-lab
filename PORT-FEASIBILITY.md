# Native-port feasibility probe (measured 2026-09-29/30)

Question: how far is `fft_decomp` from compiling with a modern compiler, as a first step toward a native
(HD / multiplayer-capable) build? Method: snapshot of `master` (`git archive`), then `gcc 12.2` from the
toolchain image over all 5,284 C files, `-std=gnu89 -funsigned-char -fcommon -nostdinc -Iinclude -w`,
one process per file. Nothing was modified in the repo. Reproduce with `.\port\probe.ps1` (scripts in `port\tools\`).

## Results

| Mode | Files passing | What it tells us |
|---|---|---|
| `-m32 -fsyntax-only` | **5,196 / 5,284 (98.3%)** | the C itself is already close to portable |
| `-m32 -S -O0` (real code generation) | 4,882 / 5,284 (92.4%) | 402 fail, almost all on MIPS register syntax |
| same, with `-D'__asm__(...)='` | 5,060 / 5,284 (95.8%) | stubbing `__asm__(...)` fixes most; `__asm__ volatile(…"$5")` clobbers remain |
| `-m64 -fsyntax-only` | **461 / 5,284** | the code is **32-bit only**: struct-size asserts (`native_thread_size_must_be_0x400`, …) fail everywhere |

The 402 code-generation failures: 382 are *only* MIPS register annotations (local `register x __asm__("$N")`
pins, `"$N"`/`lo`/`hi` clobbers, file-scope register variables in `include/psx/*.h`); 20 have a real C problem,
"conflicting type qualifiers" (a file-local `extern T volatile g_x;` view of a global that the header declares
without `volatile`). By area: effect 64, battle 64, world 41, event 33, wldcore 22, main 10, open 6 = 240 game
files; the other 160 are `src/psyq/*` (the PS1 hardware libraries a platform layer replaces anyway).

Inline asm that really emits instructions (not pins, not empty barriers):

* game code: **~17 files** (10 battle, 7 world); templates are mostly GTE loads/stores (`lwc2`/`swc2`) and
  scheduling no-ops (`move $1,$1`, `addu $1,$1,$zero`, `nop`, `.set noat`);
* 6 shared headers in `include/psx/` (`abi_inline.h`, `cpu_*`, `gte_inline.h`, `card_*`, `libpress_abi_inline.h`);
* SDK: libspu 6 files, libcd 1, crt 1 (replaced by the platform layer).

## Update: the portify transform (`port\tools\portify.py`, run via `.\port\probe.ps1`)

A build-time transform (comment/string-aware, stdlib-only Python, ~5 s for the whole tree; the repo is not touched)
removes 751 register pins (241 files) and 602 empty asm barriers/ties (250 files), turns 8 asm ties that actually *assign*
into plain assignments (see the lesson below), turns 24 file-local volatile "views" that conflict with header declarations into
self-referential macros (`#define g (*(T volatile*)&g)`; 68 more are the only declaration of their global and stay), and lists
the asm that is real. Result with `gcc -m32 -S -O0`:

| Tree | Compiles | Remaining errors |
|---|---|---|
| untransformed | 4,882 / 5,284 (92.4%) | register pins, clobbers, volatile-view conflicts |
| **after portify** | **5,166 / 5,284 (97.8%)** | only real hardware asm (`$N`/`lo`/`hi` clobbers, 1 impossible constraint) |

The 118 files that still fail: game code 70 (effect 64, battle 5, world 1) + SDK 48 (libgte 42, libcard 3, crt 3).
Game-side, the whole blocker is the **GTE layer**: ~10 macros from `include/psx/gte_inline.h` — `gte_ldv0`,
`gte_stlvnl`, `gte_stflg`, `gte_RotTrans`/`_split`, `gte_stsxy`/`stsxy3`, `gte_ldv3`, `gte_SetRotMatrix`,
`gte_SetTransMatrix`. The CPU/CD/card ABI headers (`abi_inline.h`, `cpu_*`, `card_*`, `libpress_abi_inline.h`;
~180 lines) are used only by the SDK reconstructions that the platform layer replaces. So the first native-build
milestone is: **a software GTE exposing those macros + the libgte C API** (RotTrans*, SetRotMatrix,
RotMatrix, SquareRoot12, VectorNormal, ...), then link.

Write the GTE from the public hardware documentation (or a permissively licensed reference); do not paste in
GPL emulator code without deciding on the licence consequences for the port.

## Native spikes: the game's own code running natively (`port\native\`)

**Spike 1 — `.\port\native\zodiac.ps1`.** The unmodified `battle_formula_apply_zodiac_compatibility` (portified,
`gcc -m32`, freestanding, linked with a 40-line harness and run in the container) over all 13x13 sign pairings, with the
compatibility table read from `BATTLE.BIN`. Output matches FFT's rules: opposite signs +50% (male vs female) / -50%
(same sex), trines +25%, squares -25%, monsters get the ordinary bad result, Serpentarius neutral.

**Spike 2 — `.\port\native\ram.ps1`: a PS1 RAM image at the original addresses.** On 32-bit Linux user space includes
`0x80000000`, so the harness `mmap`s 2 MiB there and loads `SCUS_942.21` (@`0x8000f800`) and `BATTLE.BIN`
(@`0x80067000`) into it; every data symbol from `target/main.yaml` + `battle.yaml` (1,713) is given its original address
through a generated linker-script include (`gen_symbols.py`). The game's own code then reads real disc data through
its own symbols with **no source changes** — the 295 game files that hardcode `0x80xxxxxx` addresses need no rewrite.
Result: `battle_formula_calculate_base_xa` computes XA/YA for every weapon in the item table; the real weapon table
comes out right (knives: Dagger WP 3, Mythril Knife 4, … Air Knife 10, Zorlin Shape 12; ninja blades 8–15; swords, knight
swords, katanas, …) and the per-type XA rules hold (knife/ninja blade `(PA+Sp)/2`, sword `PA`, knight sword/katana
`PA*Brave/100`). Full output: `port\native\OUTPUT-ram.txt`.

What this proves: (1) the portify transform is enough for game logic to compile and run; (2) a fixed-address RAM
image is a workable memory-map strategy for a 32-bit build, and it makes disc data usable as-is; (3) the game's
randomness can be made deterministic — but note `rand()` is a **BIOS** routine (not in the decomp), so a native build
supplies its own generator (see the RNG section below).

Limits and caveats:
* 32-bit only. Mapping `0x80000000` works on 32-bit Linux; on Windows a 32-bit process needs `/LARGEADDRESSAWARE`
  (64-bit OS) to reach it, or a 64-bit build with an address-translation shim instead of a fixed mapping.
* Function pointers stored in game data tables hold PS1 code addresses. Natively compiled code lives elsewhere, so
  those tables need a translation step (an id -> native function map) before the code that dispatches through them runs.
* **Correction on reachability.** A regex-based name scan (`port\tools\closure.py`) suggested the call closure of one
  damage function was ~2,900 functions; that over-approximates (identifiers in names/comments count as calls). Measured
  by actually linking (spike 3 below), the closure of the whole combat-formula layer is **244 functions** and converges.
  Reachability through *state machines* (menus, effects, camera) is a different matter: those are called through
  function-pointer tables and the `g_battle_game_state` switch, so they are pulled in by choosing modules, not by call graph.
* (Superseded: the first spikes were checked against FFT's rules only. Since 2026-09-30 there is an oracle — see "The oracle" below —
  and the formula layer now matches the original machine code in ~149,000 differential trials.)

**Spike 3 — `.\port\native\attack.ps1`: a full weapon attack resolved by the game's own code.** All 209
`battle_formula_*` functions, the generated native formula-handler table (`g_battle_formula_handlers` decoded from
`BATTLE.BIN` and translated from PS1 addresses to native functions by `decode_handlers.py`) and everything they call —
**244 source files, 0 stubs** besides `abs`/`rand` — link into a 32-bit freestanding program with the RAM image. The harness
sets up two `battle_stats_t` units by field name, a real weapon from the item table, and calls
`native_formula_handlers[1]()` (formula 1 = `battle_formula_weapon_damage`, the plain weapon attack: evade -> hit roll ->
base XA -> statuses/zodiac/critical -> damage). 2,000 trials per attack direction (attacker PA 12, Brave 70, weapon WP 12,
male Aries vs a 15%-class-evade female Libra):

| target facing | classified as | hit% | crit% | damage min / avg / max |
|---|---|---|---|---|
| 1 | front | 85 | 3 | 216 / 220 / 408 |
| 0, 2 | side | 100 | 3–4 | 216 / 219–220 / 420 |
| 3 | back | 100 | 3 | 216 / 219 / 420 |

Every figure matches FFT's rules as the source states them: the 15% class evade applies only to frontal attacks
(85% vs 100%), the direction test matches `battle_formula_calculate_facing_evade` line for line, the critical roll is
"random 0–99 < 4" (≈4%) with crits up to ~2x, and the no-crit damage is exactly `XA x YA` after the zodiac bonus:
opposite signs + opposite sexes = *best* (+50%): PA 12 -> XA 18, x WP 12 = **216**. The RNG in this run is a placeholder LCG
(the real one is a BIOS routine — verify its constants before trusting the *distribution*); everything else is the game's
own code and data. Output: `port\native\OUTPUT-attack.txt`.

What it means: the rules engine is **deterministic, self-contained and runs headlessly** — exactly the piece an
authoritative co-op/PvP host (or a lockstep peer) needs. Serving a battle without any graphics, GTE or SPU is realistic.

## The oracle: the original machine code as the reference (`port\native\r3000\`, added 2026-09-30)

Spikes 1–3 were checked against FFT's *rules*, not against the original program. No emulator is needed to close that gap: the
disc image already contains the original machine code. `port\native\r3000\` is a small (~400 lines, freestanding C) **MIPS R3000A
interpreter**: the whole MIPS-I integer set with branch- and load-delay slots and unaligned load/store, COP2 through the software
GTE (`port\native\gte\`), the BIOS entries the game's C code reaches (`rand`, `srand`, `abs`, memory/string helpers), `syscall`
critical sections as no-ops, and a fault for anything unsupported. CPU only — no GPU/SPU/CD/DMA; hardware-window accesses read as
zero and are counted so a test can tell. A **differential test** runs a function both ways on identical inputs — the native build
inside the process's mapped RAM image, and the original code on the interpreter — and compares the return value and every data byte
of the 2 MiB RAM (code ranges excluded). The native half of each trial runs in a forked child, so a native crash is caught and
classified instead of ending the run.

| Test (script in `port\native\`) | Trials | Result |
|---|---|---|
| native libgte, 35 functions incl. `RotMatrix`, `ApplyMatrixLV`, `VectorNormal`, `SquareRoot0/12`, `Mul/Scale/Push/PopMatrix`, `RotTransPers*`, `NormalClip`, plus `rsin/rcos/ratan2/csqrt` compiled from the repo (`diff_libgte.ps1`) | 128,200 | **0 differences** in return values and memory; only the scratch GTE register VZ0 differs after `MulMatrix*` |
| ~100 battle-formula handlers, called through the game's own `g_battle_formula_handlers` table (`diff_formulas.ps1 -Trials 1500`) | 149,086 | **0 differences, 0 native crashes** (random units + ability record, half fully random bytes, half plausible values) |
| native C replacements for 8 hand-assembled BATTLE routines in `replacements\battle_asm.c` (`diff_asm.ps1`) | 28,000 | **0 differences** |
| GTE unit tests, incl. the hardware-style perspective divide vs exact division (`gte\tests\run.ps1`) | 4.58 M checks | all pass |

What the oracle found or forced (each is handled in the tools):
* **Function pointers hold PS1 addresses.** The harness writes an x86 `jmp` at every natively linked function's *original* address
  (a trampoline; `gen_stubs.py` + `harness_diff_formulas.c`), so `g_battle_formula_handlers[i]()` lands on native code without patching
  any table; code ranges are excluded when RAM is copied and compared. Overlays share addresses, so a runtime must install the
  trampolines per loaded overlay.
* **A symbol-script bug.** `main.yaml` lists 35 overlay functions as bare address rows, and `gen_symbols.py` linked them at their PS1
  addresses instead of compiling them (a jump into data on the first call). Function names are now excluded from the data-symbol script.
* **Divide by zero.** x86 traps where MIPS defines a result (`lo = -1` or `1`, `hi` = the dividend); ARM would return 0. Real data does
  not hit it (3 of ~150k random states, all in formula 66), but a modded value would crash a native build, and multiplayer needs every
  peer to agree on the result. The port needs a defined behaviour at the source level (checked division), not a SIGFPE hack.
* **Random input can make the original read its own machine code as data** (a wild table index lands in the code region; natively those
  bytes are the trampolines). The interpreter now counts data loads from code ranges and such trials are skipped (853 of ~150k).
* **A divergence that looked like a real undefined-behaviour bug** (formula 37, 1 state in ~1,440: native called `rand()` twice, the original once)
  was traced with the harness's replay mode (`diff_formulas.ps1 -Replay '37,1332'`: call trace and a RAM hash at every call on both machines,
  then the RAM diff at the first diverging call, then the original's arguments) to exactly that artifact. Along the way: a `jal`'s delay slot
  runs *before* the callee starts, so state snapshots must be taken when the callee is entered, not at the `jal`.
* **Real MIPS asm has native equivalents.** `battle_copy_bytes`, `battle_find_text_id_location` and the six thread accessors are
  plain C in `replacements\battle_asm.c` (the WORLD twins are in `world_asm.c`, compiled but not yet diffed — they need the WORLD overlay in
  the interpreter's RAM). `battle_thread_get_current_global_pointer` returns `$gp`, which has no native meaning; it returns 0.

What it does **not** prove yet: both machines use the *same software GTE*, so libgte is verified against the original *algorithms*, not against
GTE hardware; both use the documented BIOS `rand` LCG (unverified against the BIOS ROM); inputs are random, not recorded battles; and only
functions whose whole call tree is native are covered (the 244-function formula layer; the other ~2,000 need SDK stubs first).

**The native link boundary** (`.\port\native\boundary.ps1`: compiles every `src/main` + `src/battle` file with the GTE shim, in parallel). 2,308 of 2,316
files compile natively; the 8 that do not are the hand-assembled routines above. The remaining **201 unresolved externals** are exactly the platform
layer a native port has to provide: GPU/primitive setup (`AddPrim`, `ClearOTag`, `DrawOTag`, `LoadImage`, `PutDispEnv`, `SetPolyF4`… ~50), SPU (~35),
CD (~16), pad/events/timers/`VSync` (~20), libc (`abs`, `bzero`, `memcpy`, `memset`, `rand`, `srand`, `SetMem`), the entry points of the *other overlays*
(`attack_*`, `bunit_entrypoint`, `equip_entrypoint`, `option_entrypoint`, `jobstts_entrypoint`… ~20), and ~12 asm-only functions. Nothing else.

## HD sprite pipeline: upscale in index space (validated on one sheet)

Unit recolours are palette swaps, so an HD pipeline must not bake one palette into RGB. `port\tools\sprite_upscale_demo.py`
upscales the sheet's **palette indices** (Scale2x/EPX applied twice = 4x), then applies palettes afterwards.
`port\samples\upscale_knight_index_space.png` shows `KNIGHT_M` with palettes 0–3 (rows), nearest 4x on the left and
index-space Scale2x x2 on the right: the smoother curves come out crisp and *every* palette recolours the result correctly
(the cloak goes white / cream / grey-red / green). Design consequences:
* keep the game's 4-bit index + 16-palette data model and add a higher-resolution index plane (or a per-sheet scale factor),
  so no game logic about palettes changes and mods keep working;
* a learned/AI super-resolution can be used *if it outputs indices* (or if each of the 16 palettes is upscaled separately —
  138 sheets x 16 = 2,208 images, the brute-force fallback);
* frame rectangles come from the `.SHP` tables (decoded, next section) so upscaled frames land where the game expects them;
  the renderer then samples the HD plane with the same UVs scaled. (The `.SEQ` animation scripts are not decoded yet.)

## Sprite frame tables (`.SHP`) decoded

Read from `battle_gfx_unpack_unit_shp_data` / `battle_gfx_load_unit_frame_parts` and verified visually:
* **SHP**: `u32` header (offset of a second table set, or 8), `u16` attack-frame start, `u16` sp2-frame start, then 0x100 `u32`
  frame offsets (only 0xd0 = **208** are used; -1 = none) relative to a frame blob (`u16` size + bytes at `base + 0x402`).
  A second set (e.g. the "submerged" variant, chosen by `battle_gfx_select_unit_shp_frame` from tile water depth) follows when
  the header is not 8.
* **Frame**: `u8` (part count − 1 in bits 0–2, Y-rotation index in bits 3–7), `u8` flags, then 4-byte parts.
* **Part**: `s8 x_shift, s8 y_shift, u16 attributes` — bits 0–9 tile (`u = (tile & 0x1f)*8`, `v = (tile >> 5)*8` in the 256-px
  sheet), bits 10–13 size index into `g_battle_gfx_part_sizes` (in `BATTLE.BIN` at `0x800946c8`; on your disc: 8x8, 16x8, 16x16,
  16x24, 24x8, 24x16, 24x24, 32x8, 32x16, 32x24, 32x32, 32x40, 40x16, 40x32, 48x48, 56x56 pixels), bits 14–15 flips.
* `g_battle_gfx_spritesheet_data[id]` = `{shp_id, seq_id, flying_flag, graphic_height}` for the 159 sprite sheet ids.

`port\tools\shp_frames.py` assembles frames from a sheet (`port\samples\frames_10M_assembled_from_TYPE1_SHP.png`: the generic male
in standing, walking and facing poses) and dumps every frame's part rectangles to JSON (`--json`; `port\build\shp\*.json`).
All seven unit-type tables (`ARUTE`, `CYOKO`, `KANZEN`, `MON`, `OTHER`, `TYPE1`, `TYPE2`) give exactly 208 frames per set (416 where
a second set exists). `WEP1/2` and `EFF1/2` use a different layout (`battle_gfx_weapon_shp_t`: 32 first-frame indices, then
frame pointers from 0x40) and are not decoded yet; the `.SEQ` animation scripts (`battle_gfx_run_unit_seq_script`) are the next
piece for a full sprite pipeline.

## Rendering facts (for the HD renderer)

* Native framebuffer in battle: **256x240**, two buffers stacked in VRAM (`SetDefDrawEnv/DispEnv`), projection
  distance `H = 0x200` (`battle_state_init_deployment_display(0x100, 0xf0, 0x200, …)`). The display code also
  supports a 480-line interlaced mode (`height != 0x1e0` selects the second buffer's Y), so size is a parameter,
  not a constant.
* The game draws with a **small GPU primitive set** (files using each): `POLY_GT4` 203, `POLY_FT4` 174,
  `SPRT` 66, `DR_MODE` 39, `POLY_G4` 18, `POLY_F4` 17, `TILE` 22, `DR_MOVE` 15, `LINE_F2` 14, `LINE_G2` 7,
  `POLY_F3` 6, `POLY_FT3`/`POLY_GT3` 3 each, `DR_TPAGE` 3, `POLY_G3` 2, `LINE_G3` 1. Never used:
  `SPRT_8/16`, `TILE_1/8/16`, `DR_ENV`, `DR_LOAD`.
  => a hardware renderer needs textured/gouraud quads and triangles, sprites, flat tiles, lines, draw-mode /
  texture-page state and VRAM-to-VRAM moves. That is the classic minimal PS1 GPU subset.
* 3D goes through the GTE (see above), so a float / high-precision GTE path (PGXP-style) is where widescreen,
  stable geometry and perspective-correct texturing come from.

## Art inventory (what an HD pass would actually touch)

Measured from the 2,465 files the build extracts from your disc (`fft_decomp\build\extracted\files`, 450 MiB):

| Area | Files | Size | Notes |
|---|---|---|---|
| Video: `OPEN/*.STR` (6 FMVs) + `ENDING.XA` | 7 | 294 MiB | 65% of the disc, but only re-encoding/upscaling work, not redrawing |
| `MAP/` | 1,575 | 89.5 MiB | map meshes + textures (numbered extensions); format not yet decoded |
| `WORLD/` | 8 | 22.9 MiB | `WLDBK.BIN` (15 MiB) is the world-map background |
| `EFFECT/` | 512 | 19.2 MiB | ability effects (meshes, particles) |
| `EVENT/` | 44 | 11.7 MiB | `EVTCHR.BIN` 4 MiB, scripts |
| `BATTLE/` | 184 | 6.7 MiB | includes **138 unit sprite sheets** (`*.SPR`, 5.8 MiB) |
| `MENU/` | 24 | 0.6 MiB | UI graphics are tiny |

Unit sprites (decoded and checked visually with `port\tools\spr2png.py`): 512-byte palette block (16 palettes x 16
colours, 15-bit BGR, colour 0 transparent) followed by a 256-pixel-wide 4-bit image; the common size is 37,377 bytes
= 256x288. Each character frame is ~24x40 pixels, ~15 frames per sheet, and the 16 palettes are the team/job colour
swaps. So the character art is **138 sheets of ~24x40 frames** — small enough for AI upscaling plus hand cleanup,
provided the palette-swap mechanism is preserved (recolour = palette swap, not separate art).

`port\tools\sprites_all.py` decodes every sheet (2.7 s for all 138) into `port\build\sprites\*.png` plus
`_contact.png`; checked visually, the layout holds for **all** of them: generic jobs in M/W variants (`10M`, `KNIGHT_W`,
`NINJA_M`, …), story characters (`CLOUD`, `RAMUZA`, `AGURI`, …), monsters (`BEHI`, `DORA1/2`, `HASYU`, `MINOTA`, …), a
weapons sheet (`WEP`) and an icon sheet (`OTHER`). Heights are 288 (13 sheets, 37,377 bytes) or ~334–344 (the rest,
~43 KB); the taller ones carry extra rows. `DAMI`, `KASANEK`, `KASANEM` render as placeholder-looking patterns in palette 0
(masks/effect sprites). The sprite frame tables (`.SHP`) are decoded (next sections); not yet decoded: the `.SEQ` animation
scripts, the weapon/effect frame tables, and the map/effect/world formats.

## Determinism hazards found in `QUIRKS.md` (matters for a native port and for lockstep netplay)

On the PS1 these bugs are *deterministic by accident* (a stale register or uninitialised stack slot always holds
the same value in the same call path). Compiled natively they are undefined behaviour, and two machines can then
diverge on identical input — fatal for lockstep multiplayer. Each needs an explicit, documented replacement value:

* stale-register / missing-argument calls: `world_menu_resize_parent_entry_to_digits` (stale `$a0`),
  `main_party_save_unit` -> `main_party_remove_unit` (stale `$a0`), `battle_target_set_panels_for_action`
  (reads `$v0` after a void call), `battle_ai_evaluate_reflected_target_origins` (depends on caller's `$s2`),
  `equip_unit_load_selected_data` (extra args), `world_text_render_id_list_to_image_rows` (unread 9th arg);
* uninitialised locals: `battle_script_toggle_message_portrait_flip` / `world_script_toggle_message_portrait_flip`
  (record index never assigned, "keep the local uninitialized"), `battle_map_light_state_command` (several arms return
  an uninitialised pointer), `battle_map_blend_ambient_light_color` / `_darkness_color` (modes >= 11 leave the target
  colour uninitialised), `StCdInterrupt` (copies 4 uninitialised bytes; SDK layer, replaced anyway);
* out-of-range table reads: `battle_ai_build_targetable_tile_mask` (radii 8–15 index past the template table into
  the following status weights), `StartRCnt`/`StopRCnt` (SDK).

**The random-number streams (checked in the source).**
* `rand()`/`srand()` are **BIOS services** (A-table entries; `src/psyq/libapi/rand.c` is a 12-byte tail-call veneer), so
  the generator itself is *not* in the decomp and a native port must reimplement it. It is a linear congruential
  generator; public documentation gives multiplier `0x41C64E6D`, increment `0x3039` — **verify the constants and the output
  bit-range against the BIOS ROM or an emulator trace before relying on them.**
* Seeding: `srand(1)` at boot (`main_boot_run_startup`), reseeded from play time in `world_unit_update_monster_breeding`.
* **The VSync handler calls `rand()` every vertical blank** (`main_system_handle_vsync_callback`, 60 Hz), so the RNG state
  is a function of elapsed frames — *including load and fade time* — not just of player actions. For lockstep either tie
  that advance to the simulation's own frame counter (and count only synchronised frames), or snapshot/broadcast the RNG
  state at every sync point. See `ARCHITECTURE.md`.
* **One stream serves both gameplay and cosmetics.** Gameplay: `battle_formula_get_random_0_7fff` (damage/hit rolls; returns
  a fixed 0x4000 unless the action state is EXECUTE, so previews are deterministic), `battle_action_init_current_ability_strike_data`,
  the `battle_ai_*` choices, `battle_load_entd_units` (random unit presence). Cosmetic: `battle_effect_spawn_*`,
  `battle_effect_spell_charge_secondary_handler`, `battle_map_draw_mesh_and_weather` (rain/snow sprites, `rand() % 480`),
  `battle_effect_get_random_between*`. A client that renders effects differently (an HD renderer with different particle
  counts, a skipped animation) consumes the shared stream differently and **desyncs**. For netplay split them: a shared,
  host-seeded gameplay generator and a client-local cosmetic one.
* Two more generators: the event-script RNG `g_world_event_random_seed`, seeded from `VSync(-1)` by
  `world_script_seed_event_rng_from_vsync` (time-dependent: the host must choose and distribute the seed), and the sound
  driver's `g_main_smd_random_state` (audio only).

**The BIOS surface (56 distinct services, from `PSYQ_BIOS_*` uses).** `rand`/`srand`; string/memory (`strlen`, `strcpy`,
`strcmp`, `strcat`, `memcpy`, `memset`, `memchr`, `bcopy`, `bzero`); `setjmp`; `printf`; heap init; `GPU_cw`, cache flush;
the memory-card file API (open/read/write/seek/format/delete/next-file, card init/start/stop/status/load/info);
controller (`PadInit2`, `PadDr`, stop); the event API (open/close/enable/disable/wait/test/deliver); exception-handler
hooks (`HookEntryInt`, `ResetEntryInt`, `ReturnFromException`). All are replaceable natively; the memory-card ones map to
the port's save-game layer.

**Systematic scan (`.\port\hazards.ps1` -> `port\HAZARDS.md`).** `gcc -m32 -O2` with the undefined-behaviour warnings over
the portified tree: 619 distinct candidates (after fixing the transform bug below) — `-Wreturn-type` 242 (non-void functions
with a path that returns nothing, i.e. the PS1 value is whatever `$v0` held), `-Wmaybe-uninitialized` 196, `-Warray-bounds` 139,
**definite** `-Wuninitialized` 14, `-Waggressive-loop-optimizations` 8 (loops GCC can prove run past their array), and a genuine
C **sequence-point** violation (`battle_effect_reflect_secondary_handler.c:85,89`). Beyond `QUIRKS.md`, definite
uninitialised uses appear in `battle_camera_reset_script_transform`, `battle_map_dispatch_gns_resource`,
`battle_menu_animate_window_quad_crop` (+ the two WORLD twins) and `battle_sound_stop_weather_sfx` (all read by hand below);
`battle_gfx_draw_map_selection_cursor` is a false positive. gcc has false positives; treat the rest as a worklist.

**What the scan is made of (sites read by hand).** Most `-Warray-bounds` / `-Waggressive-loop-optimizations` hits are
*declaration artifacts*, not retail bugs: `g_battle_menu_ability_display_flags[20]` is cleared by an 80-iteration loop,
`g_main_terrain_movement_cost_tables[][64]` is read across 256 entries, `color[0][i]` (i < 6) is used as flat storage, and
`main_unit_calculate_entd_data`/`main_unit_set_equipment_attributes` walk contiguous struct members through a base-plus-offset
view. Fixing those declarations would change the byte-exact indexing, so they stay; natively they are neutralised by
flags. The ones that need *code* attention are: the sequence-point violation in `battle_effect_reflect_secondary_handler`
(`work->clut[b][((lit = timer - 1) - span) * 2] = work->clut[b][(lit - span) * 2 + 1] = 0xffff;` — hoist the assignment),
uninitialised arguments (`battle_camera_reset_script_transform` passes an unassigned `buffer`;
`battle_menu_animate_window_quad_crop` and its WORLD twins pass an unassigned `source`, documented in the source as
"the target passes an uninitialised $s5"), functions returning an indeterminate value (`battle_sound_stop_weather_sfx`,
documented; `battle_map_dispatch_gns_resource`, result unused by its caller), and the cases already in `QUIRKS.md`.
`battle_gfx_draw_map_selection_cursor` is a **false positive** (`bob` is initialised; the warning comes from the
`*(u16*)&bob[i]` punning read).

**Recommended native build flags** (verified to compile with the toolchain's gcc 12): `-m32 -std=gnu89 -funsigned-char
-fcommon -fno-strict-aliasing -fno-aggressive-loop-optimizations -fwrapv -ftrivial-auto-var-init=zero`. The decomp is full of type-punning (`*(u16*)&s32_array[i]`,
struct views over raw buffers), so strict aliasing must be off; signed overflow must wrap as it did on MIPS; and zero-initialising
automatic variables makes every "uninitialised local" retail bug read the same deterministic value on every peer — for lockstep
netplay peers only need to *agree*, not to reproduce the PS1's stale registers.

**Lesson recorded in the transform itself.** An "empty" `asm` is not always a barrier: `__asm__("" : "=r"(instruction) :
"0"(&g_wldcore_opcode_instruction))` *assigns*. The first `portify` deleted it (compile still succeeded; only
`-Wuninitialized` exposed it). It now converts tied-output-from-a-different-expression asm into a plain assignment
(8 sites: 5 SDK, 3 game — `wldcore_opcode_add_counter_delta_if_choice`, `world_formation_run_dismiss_unit_menu_step`,
`world_menu_open_scrollable_list`), initialises `$0`-pinned variables to 0, and lists output-only asm (25 sites,
mostly SDK) whose variable is left undefined. Compiling is not evidence of correctness: run the hazard scan after every
change to the transform.

Also worth knowing before touching gameplay: the AI has real logic bugs (`battle_ai_load_ability_entry` tests the
wrong support bits for CT; `battle_ai_evaluate_status_cancellation` Blood Suck tests a Jump-derived bit;
`battle_ai_build_monster_skill_tile_mask` only keeps +x/+y neighbours) and the camera adds a component twice
(`battle_camera_step_real_coords_toward_target`). A modder chooses per bug whether to preserve or fix; for netplay
every peer must make the same choice. Twins that must stay in sync: BATTLE/WORLD event interpreters and the
32 groups of byte-identical EFFECT routines.

## State size and rollback (design note)

With the RAM-image design (all game globals live in one 2 MiB block at their original addresses), a **save state is a
memcpy of 2 MiB** — microseconds — which is what emulator-style rollback netcode needs. A rough symbol-gap estimate
(`port\tools\memmap.py`) shows the *simulation* state is small (units ~16 KiB, AI ~8 KiB, action/script/camera state
<1 KiB each, map ~170 KiB); the bulk is presentation buffers (gfx ~310 KiB, text ~270 KiB, menu ~200 KiB).
Caveats that decide whether snapshots are exact:
* The game's **cooperative threads** keep their stacks inside RAM slots (`native_thread_t`: 16 slots x 0x400 bytes, stack
  top at +0x3f0, asm context switch). A native port that runs them on host fibers puts stacks *outside* the snapshot —
  either run them on RAM-resident custom stacks (big enough for x86 frames) or convert thread bodies to explicit state
  machines.
* Timing must be frame-driven, not wall-clock: the game reads `VSync` counters and seeds the event RNG from
  `VSync(-1)` (see the RNG section); a lockstep/rollback host must supply those values.
* Snapshot only what the simulation needs if bandwidth matters (state hash of units + RNG + script variables for
  desync detection; full RAM only for local rollback).

## What this means for a native build

1. Build **32-bit first** (`-m32`) with the recommended flags. Going 64-bit means turning pointer fields into 32-bit handles: a
   separate, later refactor.
2. Do not edit the sources for portability. Generate portable copies at build time (`portify.py`) — the matching decomp
   stays byte-exact upstream, the port lives downstream. (Done; it also handles the volatile-view conflicts.)
3. **Software GTE + libgte API** (12 GTE commands, 33 game-facing functions) so the last 70 game files compile — **done**
   (`port\native\gte\`, verified against the original code; with the shim, 4,876 of 4,878 game+main files compile natively, the two
   failures being hand-assembled text-scan loops that now have C replacements); decide the precision policy (integer-exact for
   determinism, plus a float path for HD geometry).
4. Everything else is the platform layer: GPU ordering tables -> renderer, SPU/Suzuki sound driver, CD, memory card, BIOS
   services, the cooperative-thread context switch (RAM-resident coroutines), one native module per overlay, and the 41
   game files that poke hardware registers. The 295 files with hardcoded `0x80xxxxxx` addresses need nothing beyond the RAM image.

What is proven: game logic compiles and runs natively against real disc data (spikes 1–3), and the native formula layer and libgte
match the *original machine code* bit for bit on ~277,000 differential trials (the oracle above). Remaining unknowns are runtime
ones (overlay switching, thread switching, timing) and fidelity ones (GTE hardware, BIOS `rand`, recorded-battle traces — the latter
two would need an emulator or a console capture; everything else is covered without one) — see `ROADMAP.md`.
