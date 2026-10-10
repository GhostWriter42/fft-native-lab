# Native runtime: design decisions and the evidence behind them (2026-09-30)

What a native build of the game (the base for the HD renderer and for netplay) has to look like, and why. Every claim here was
checked against the **original machine code** with the R3000 oracle (`port\native\r3000\`, see `PORT-FEASIBILITY.md`); the
scripts to reproduce are in `port\README.md`. Status of the code lives in `port\native\`.

## 1. One RAM image, canonical PS1 addresses

* The game's 2 MiB of RAM is a real 2 MiB mapping at `0x80000000` (a 32-bit process; 64-bit needs an address-translation shim).
  Every global lives at its original address (`symbols_pc.ld`, generated from `target\*.yaml`), so the 295 files that hard-code
  `0x80xxxxxx` addresses need no change and a save state is a `memcpy` of the image.
* **Every natively compiled function is linked as `native_<name>`.** The original names are bound to their PS1 addresses by the
  linker script, and an x86 `jmp` ("trampoline") written at each PS1 address reaches the native code. Consequences: taking a function
  pointer, storing it in a thread record or callback table, or dispatching through `g_battle_formula_handlers[i]` all use the
  *PS1 address*; RAM never contains a native address (the oracle found native addresses leaking into RAM before this scheme: thread
  records, `g_battle_thread_call_target`). That is what makes RAM snapshots, state hashes and lockstep comparison
  build-independent. Overlays share addresses, so trampolines are (re)installed per loaded overlay.
* Code ranges are excluded when RAM is compared: they hold trampolines natively and MIPS code in the oracle.

## 2. Cooperative threads

Retail: 16 records of 0x400 bytes (`g_battle_thread_contexts`), a handwritten switch (`battle_thread_yield`, `0x8014ca80`) that saves
`s0-s7,k0,k1,gp,sp,fp,ra` into the current record, advances `g_battle_current_thread_id` to the next record whose `is_running`
(+0x48) is set (wrapping to thread 0), calls `battle_script_route_event_input`, and restores the next record. A thread starts because
`battle_thread_start` stores the entry point in the saved-`ra` slot (`code_pointer`, +0x44). WORLD has the same switch with **17** slots.

Native (`port\native\replacements\battle_thread.c`): identical scheduling on the RAM records, so every function that reads
`is_running`, `task_id` or the parameters sees the same values; the machine context (`esp/ebp/ebx/esi/edi` and a 256 KiB private
stack per slot) lives *outside* RAM. The saved-register words (+0x10..+0x47 of each record) are therefore not maintained natively
(the oracle ignores them). A rollback snapshot must include the per-slot native contexts and the used part of their stacks.

## 3. Integer division

MIPS defines division by zero (`lo = -1` for a non-negative dividend, `+1` for a negative one, `hi = dividend`; unsigned: `lo = 0xffffffff`)
and `INT_MIN / -1` (= `INT_MIN`). x86 raises `#DE`, ARM returns 0. Real game data does not divide by zero in the formulas tested, but mods
can, and lockstep peers must agree. `port\tools\divfix.awk` expands every `idiv`/`div` by a non-constant divisor (285 source lines in 113
files of `src/main` + `src/battle`, listed by `port\tools\div_sites.sh`) into a guarded sequence with the MIPS results; the oracle
confirmed that the functions that diverged before (formula 66, the battle-effect trajectory tracers, `battle_calculate_unit_height_data`)
now match the original, including the zero-divisor trials. This is an assembly-level fix for the x86 build; a source- or IR-level equivalent is
needed for other targets.

## 4. Undefined behaviour policy

* `-ftrivial-auto-var-init=zero`: uninitialised locals read 0 on every peer. The oracle shows the retail values are *deterministic leftovers* of
  earlier calls (stale stack), which cannot be reproduced natively; where the code copies such bytes into game state (e.g. the 10-byte
  caster/target blocks in `battle_effect_init_secondary`) native and retail differ in fields that are never used, and peers agree with each other.
* Functions that can fall off the end of a non-void function (`port\tools\return_hazards.sh`: 19 in main+battle) return whatever a register held;
  natively that is unspecified. Callers ignore it in every case checked; the oracle reports these separately.
* `-fno-strict-aliasing -fwrapv -fno-aggressive-loop-optimizations -funsigned-char -fcommon -std=gnu89 -m32`.

## 5. The SDK

332 of the 339 source files under `src/psyq` (libgpu, libc, libapi, libetc, libcd, libspu, libcard, libpress, suzuki) already compile natively;
libgte is replaced by `port\native\gte` (software GTE + native libgte, verified against the original code). What remains is the *hardware*:
functions that write GPU/SPU/CD/timer registers or wait on status bits. The design is HLE at the API level (`DrawOTag`, `LoadImage`,
`CdRead`, `VSync`, `PadRead`, ...) rather than register emulation, because `DrawOTag` is exactly where an HD renderer walks the ordering
tables that the game builds in RAM.

## 6. Hand-written assembly in the retail binary (`kind: handwritten` in the yaml)

Only 22 routines in the whole game: main 1, BATTLE 13, WORLD 7, WLDCORE 1 (EFFECT, EVENT, OPEN none).

| Routine (BATTLE) | Native status |
|---|---|
| `battle_copy_bytes`, `battle_find_text_id_location`, `battle_thread_get_current_parameter_1..3`, `battle_thread_is_previous_running`, `battle_thread_is_running_8014cc94` | C, verified against the original (28,000 trials, 0 differences) |
| `battle_mul_div_s64` (64-round shift/subtract divide with retail quirks), `battle_fixed_cross_product_q12`, `battle_clear_menu_render_buffer` | C, verified (44,000 trials, 0 differences) |
| `battle_thread_yield`, `battle_thread_start`, `battle_thread_call_on_main_stack` | native coroutine scheduler / direct call (section 2) |
| `battle_map_queue_textured/untextured_triangles/quads` (4 GTE polygon queuers, ~3 KB), `blit_text_glyph`, `battle_text_render_glyph_to_4bpp_image` | not yet: presentation code the HD renderer replaces |
| `battle_thread_get_current_global_pointer` | returns 0 (`$gp` has no native meaning) |

WORLD has twins of the thread switch, divide, multiply, blitters and zero-fill (`world_asm.c` has the trivial ones).

## 7. Function-level differential fuzz (`port\native\fuzz.ps1`)

Every decompiled function of `src/main` + `src/battle` whose signature is simple (2,314 of 2,721) is called with random scalar arguments and
pointer arguments into random-filled buffers, with the ~150 pointer globals seeded to their own random pointee buffers and ~600 scalar globals
randomly perturbed, on the original code (interpreter) and on the native build; return value and every data byte of RAM must match. Trials in
which the original touched hardware, called an SDK routine, read its own machine code as data, wrote into code, used an address a native build
cannot reach, or ran too long are skipped.

Result (30 trials per function, `-AutoInit -DivFix`, PS1-address scheme): **1,871 of 2,314 functions compared at least once (81%)**, 42,691 compared
trials; 25 functions differ in some trial. Every one was replayed (call trace + RAM hash on both machines) and falls into one of these classes --
none is a mistake in the portified game logic:

| Class | Functions | Cause |
|---|---|---|
| Stale-register arguments (documented retail quirks) | `main_party_save_unit`, `battle_gfx_set_thrown_item_graphic_palette`, `..._by_battle_id`, `..._by_misc_id`, `battle_state_enter_effect_playback`, `battle_status_resolve_unit_changes_in_preview`, `battle_unit_start_post_attack_animation_display` (native crash: the callee dereferences the garbage) | the retail code calls a function without setting an argument and the callee reads the caller's leftover `$a0`/`$a1` (`get_item_data_pointer()` is `main_item_get_data_pointer(item_id)`; `battle_effect_init_data(result)` returns `$a0` for unhandled states). Natively the argument is whatever the stack holds. Needs a per-site patch (pass the value explicitly) |
| Uninitialised init-struct bytes copied into game state | `battle_action_report_level_up`, `..._job_level_up`, `battle_unit_update_animation_for_status_changes`, `battle_script_teleportin_event_instruction`, `battle_script_process_pending_requests` | the caller's `battle_effect_secondary_init_t` is only partly filled and the callee copies all of it (unused caster/target block bytes, `target_count` garbage in one path); retail copies stale stack bytes |
| Unspecified return value | `battle_move_has_reached_*` (3), `battle_unit_animate/advance_*teleport_distortion` (2), `battle_menu_alloc_window_buffer_pair`, `battle_script_set_units_movement_effect_suppression`, `battle_map_control_gte_background_color`, `battle_action_calculate_at_list_and_get_specific_unit_id`, `battle_ai_can_unit_be_targeted_cryst_trea_mount_trans` | the function can fall off its end / return an uninitialised local; RAM and calls are identical |
| Thread machinery | `battle_menu_build_ability_preview_at_list`, `battle_menu_check_action_slot_restrictions` | `battle_thread_call_on_main_stack` saves `$sp`/`$ra` in RAM; the native call does not. The second one calls `battle_thread_start(-1, ...)`: retail writes the record before `g_battle_threads`, the native replacement also indexed its context array with -1 and crashed -- a real bug in the replacement, fixed (bounds check) |
| Overlay switching | `main_overlay_call_battle_entrypoint` | jumps into an overlay entry point (not part of this build) |

Two further findings: `-ftrivial-auto-var-init=zero` is **not** a guarantee at -O1 (GCC 12 still folds the uninitialised return of
`battle_map_control_gte_background_color` into the one value that is assigned), so cross-compiler determinism needs a source-level zero-initialisation
pass; and calling an *alias* symbol of a function (`get_item_data_pointer`) works only because the trampoline sits at the shared address.

Method notes worth keeping:
* the interpreter must start every trial clean (registers, stack region, code image, RAM from the pristine disc image), otherwise one
  trial's wild write changes the next trial's behaviour;
* a divergence is localised by `-Replay 'index,trial'`: call trace + a hash of all data RAM at every call on both machines, the RAM words that
  differ when the first diverging call is entered, and the original's arguments. (A `jal`'s delay slot runs before the callee starts: snapshots
  must be taken on entry, not at the `jal`.)

## 8. Headless boot of the ORIGINAL game on the interpreter (`port\native\boot.ps1`)

A prototype of the platform layer: the original `SCUS_942.21` is started at `__SN_ENTRY_POINT` on the R3000 interpreter, the SDK's hardware layer
(everything in the libspu / libetc / libcd / libcard address ranges, the BIOS event API and the hardware-facing libgpu entry points) is replaced by
HLE hooks written in C, and the disc is read by LBA from the raw image. About twenty behaviours are needed to get through the boot:
`VSync` (advance the frame counter and run the game's own vertical-blank callback), the five callback registrars, `CdInit`, `CdIntToPos`/`CdControl`
(position -> LBA), `CdRead` (copy the sectors, then run the registered read callback immediately), `CdSync`/`CdReadSync`, `SpuSetTransferCallback` +
`SpuWrite` (finish the transfer at once and run the callback; the sound driver otherwise waits forever on a RAM flag), the rest return 0.

Result (no game code was changed): the original code runs the whole start-up sequence, draws its logo frames through the real libgpu primitive
setters, loads its sound data and system files from the disc (10 sectors at LBA 198, 242 at 85007, then a series of small reads and larger ones at
LBA 60513 / 3688 / 86000..86595), and reaches the OPEN overlay (opening movie / title) at roughly frame 490. It then spins on `CdRead2` (the
streaming API used for the FMVs), which is the first thing the prototype does not implement.

Why this matters: the same C HLE source can be compiled twice, once against the interpreter's RAM and once against the native RAM image, so the
native game and the original code can be run frame by frame from the same boot and their RAM compared at every `VSync` -- a whole-program
differential test, and the natural first milestone of the native runtime ("boots headless to the opening overlay"). The native side already has the
pieces it needs: the RAM image, trampolines, coroutine threads, the software GTE.

## 9. Whole-program lockstep (`port\native\lockstep.ps1`)

The HLE of section 8 now lives in one shared source (`port\native\hle\hle.c`, generated glue `gen_hle.py`) that is compiled twice: against the
interpreter's RAM and against the native RAM image. `lockstep.c` runs both machines from the entry of `main()`:

1. the original code runs from `__SN_ENTRY_POINT` on the interpreter until it reaches `main()` (71,514 instructions: BSS clear, `__SN_ENTRY_POINT`);
2. the interpreter's data RAM is copied into the native image (function bodies stay as x86 trampolines) and the native `main()` starts on a coroutine
   stack; the GTE state is swapped per machine at every switch;
3. per frame: native runs to its next `VSync(0)` (the HLE runs the registered vertical-blank callback, then switches back to the driver), the
   interpreter runs to its next `VSync(0)`, then **every data word of RAM (88 ranges, 1.3 MB) and the scratchpad are compared**. The first mismatch
   is printed with labelled addresses; a native crash prints a frame-pointer backtrace resolved through `nm`.

What is linked natively: `src/main`, `src/battle`, and every SDK source that is not hardware-facing (libgpu setters, libc, parts of libapi ...).
Dropped from the link: the objects that define SDK functions the HLE takes over (220 names; libspu/libetc/libcd/libcard and the event/pad/GPU
entry points) and the 54 BIOS tail veneers (`li t2,0xa0; jr t2; li t1,N` -- natively they would "goto" address 0xa0). The BIOS services the game
uses are provided by `bios_rt.c` with the same semantics as the interpreter's BIOS layer; anything else that has no native definition becomes a
counting stub whose calls are reported (none in the boot).

**Result 1: 445 frames (VSyncs) from `main()` to the first code-overlay load (OPEN/OPEN.BIN, LBA 86000), RAM identical at every one, zero stub calls.**
The boot covers the SN crt state, the logos, sound-driver initialisation, system-file loading (11 CD reads with the game's own sector/callback
handling), the game loop and the soft-reset machinery.

**Result 2 (overlays, input, platform traffic):** the lockstep now continues across code overlays. `gen_modules.py` writes a registry of the modules
(main, battle, opening, wldcore, world; one entry per yaml document -- event.yaml has 11 and effect.yaml 110, one per overlay file) with their disc
position, load address and function table. When a `CdRead` lands on a module's file the two machines stop, their RAM is compared, the module's
functions get x86 trampolines (any module whose address range it overlaps is unloaded: its trampolines are restored from the interpreter's RAM, its HLE
hooks are removed), its data ranges become part of the comparison and the run continues. With `-Scenario title` (the game loop is steered past the
opening movie -- whose CD-streaming/MDEC hardware is not modelled -- by storing 5 into `g_main_system_frontend_world_result` right after
`main_item_init_new_game_inventory`, on both machines) and a scripted controller (`-Pad 'frame:buttons,...'`), **the game boots, loads OPEN.BIN, shows
its title menu, takes START, plays the new-game transition and loads WORLD.BIN for the name-entry screen -- 950 frames, RAM identical at every VSync
and at both overlay loads, OPEN natively (150 of 150 functions) and WORLD natively (1,012 of 1,012)**.

Everything the machines do at the platform boundary is compared too, not just RAM: the HLE keeps a per-frame log of its calls (`hle_trace_entry_t`) with
each function's real argument count (`gen_hle.py` reads it from the SDK sources/headers; the other argument registers hold garbage) and pointer
arguments replaced by the *content* they point at: RECT / DISPENV / DRAWENV / CdlLOC / SPU attribute structs and whole GPU ordering tables (the packet
chain of `DrawOTag`, addresses excluded), stack pointers canonicalised. Calls the original makes from inside the natively replaced libgte (critical
sections, `FlushCache`) have no native counterpart and are not logged. This is exactly the interface an HD renderer / audio backend will consume, so
verifying it is verifying the port.

What the higher-level HLE had to learn to get this far: `ClearOTagR`'s DMA channel (`_otc`), the BIOS event system (`OpenEvent`/`TestEvent`/
`DeliverEvent`, 32 events) with a console that has **no memory card** (every card command is accepted and answered with the software-card TIMEOUT event --
the game's card polling otherwise never terminates), and scripted `PadRead`. The native game runs on a stack **inside the RAM image** (top 64 KiB,
like the console's) because code such as the name-entry screen builds GPU ordering tables from stack locals whose 24-bit addresses must stay valid.
At this stage the render-only hand-written routines (BATTLE's four polygon queuers and two glyph blitters, WORLD's two blitters) were *skipped on both machines*
(`HLE_SKIP_NAMES`: the interpreter never ran the original bytes and the native side got a no-op) so that the rest of the game could be compared before they were
transliterated; they all have native versions now (see Result 4) and only the FMV start is still skipped.

**Result 3: real play.** `-TitleToBattle` (START/CIRCLE presses, frames 600-1180) takes a new game into its first battle at frame ~1170; from there random
controller input (`padgen.ps1`: d-pad runs, confirm, cancel, menu, camera, weighted like menu navigation; every seed is a different game) drives it through unit
menus, movement, attacks, the AI's turns and the EVENT overlays (BUNIT, EQUIP, JOBSTTS, HELPMENU, OPTION, REQUIRE ... are loaded by the game's own menus and run
natively too). With a cheat that ends the first battle (`-PokeWhen`, see below) the same harness reaches the **world map with the WLDCORE + WORLD overlays** (menus, shops,
formation, tutorials). `soak.ps1` runs one docker container per seed, 12-16 in parallel, each a complete lockstep (RAM at every VSync; VRAM with `-Gpu`):

| Soak | Seeds x frames | Result |
|---|---|---|
| title -> battle, random play | 40 x 12,000 | all identical (RAM and VRAM), 74 s wall on 12 parallel runs |
| world map (poked), random play | 24 x 9,000 | all identical (RAM and VRAM) |
| title -> battle, 30,000-frame button mash | 1 | identical |
| title -> battle, long | 40 x 60,000 | **40 identical** (seeds 201-240; an earlier run had 35, the 5 others were dead stack garbage, see "Comparison rules"). Function coverage of these seeds: 2,183 of 5,318 functions (battle 1,353 / 1,912, main 328 / 821, effect overlays 2 / 210) |

Function coverage of the native game (union over the soak seeds, `build\soak\coverage.txt`): main 325/821, battle 1,247/1,912, opening 70/150, event overlays 272/777,
world 608/1,013, wldcore 138/435; effect overlays are hardly reached by random play (an ability with an effect must be used: 3 EFFECT loads in ~10 long seeds).

What the platform layer had to learn for this (all in `hle/hle.c`, `lockstep.c`, `r3000/`): CD streaming (`CdRead2`/`CdReady`/`CdGetSector`/`CdDataSync`/`CdStatus`), the
`VSync(1)` scanline counter (a function of the call history, identical on both machines), tick hooks (virtual vblanks inside polling loops that contain no SDK call),
the BIOS event system with a no-card console, thread stacks and the main stack inside the mapped RAM window (`thread_window.h`: their addresses go into GPU ordering tables),
the zero-filled stack frames of the oracle (`zero_frames`, the native side is compiled with `-ftrivial-auto-var-init=zero`), an optional mapped NULL page that logs the
places where retail reads console RAM through a NULL pointer, per-function coverage thunks.

**Result 4: pictures.** `hle/gpu.c` is a software model of the PlayStation GPU (1024x512 VRAM, drawing environment, flat/gouraud/textured triangles and quads, sprites and
tiles, lines, fills and VRAM copies, 4/8/15-bit textures through CLUTs, texture window, the four semi-transparency modes). The HLE feeds it the calls the real GPU would
get on BOTH machines (`DrawOTag`, `LoadImage`, `StoreImage`, `MoveImage`, `ClearImage`, `PutDrawEnv`, `PutDispEnv`), so the original machine code and the native build each draw
into their own VRAM; `-Gpu` compares the two VRAMs at every frame (a pixel counts when the display shows it or either machine has read it as texture/CLUT/StoreImage data: the
unread rest of an uploaded buffer is uninitialised stack on retail) and `-Shot` writes the native display as PNG. **The native game draws the title screen, the memory-card
warning, the location banners, the opening event's dialogue with its sprites, the world map with its menus (Move / Formation / Brave Story / Tutorial / Data / Option, war
funds, the party marker) and the first battle (map polygons, units, menus, status panel, rain), bit-identical with what the original code draws.** Pictures:
`port\samples\native-frames\`. Getting there needed native versions of the hand-assembled render routines that the lockstep used to skip: the four text blitters are
literal transliterations (`replacements/world_asm.c`, `battle_asm2.c`); the four GTE map-polygon queuers (188-220 instructions each) are generated from the machine code by
`tools/mips2c.py` (`replacements/battle_asm3.c`), a small static binary translator (delay slots, GTE through the software GTE, load-delay hazards reported) that also works for
any other leaf routine. Two GPU details the game depends on, found by looking at the pictures: libgpu clamps the clip rectangle of a DRAWENV (the deployment screens ask for
x = -128: clamped to 0; masking it wrapped pixels into the texture pages at the right edge of the VRAM and striped every wall), and `StoreImage` must return what is in VRAM.

The VRAM itself stays plain 1x (the game reads it back); the HD prototype below keeps a second, finer copy of the two display buffers.

**Result 5: the native game alone -- playable, savable, deterministic** (2026-09-30 night).

* **Native-only mode** (`-NativeOnly` / run.cfg `nativeonly 1`): the original still boots on the interpreter up to `main()` (its RAM is copied into the native image), then only
  the native game runs -- no interpreter, no comparison. 11,000 frames of title -> first battle with the software GPU take 42 s (~260 frames/s, one core). Overlay loads still
  install / evict modules: the five code bytes each x86 trampoline replaces are kept (`g_saved5`) because there is no interpreter RAM to take them back from.
* **Play mode** (`play.ps1` -> `play.py` <-> docker container, over the container's stdin/stdout): every frame the driver sends `'F' 'R' w h frame` + RGB, the viewer answers with
  three bytes (the pad, 16 bits, and a command byte). Keys: arrows, Z Cross, X Circle, A Square, S Triangle, Q/W L1/R1, E/R L2/R2, Enter Start, Backspace Select; P pauses
  (the game is frame-driven by the viewer's answers), Tab fast-forwards, F12 saves a PNG, **F1-F4 save the whole machine state to a slot, F5-F8 load it**. `-Verify` (`play 1`)
  runs the original machine code alongside and compares RAM and VRAM every frame while you play. `play_test.py` exercises the protocol without a window (both modes pass,
  1,300 frames, save + load). The Tk window itself has not been exercised yet; no sound, movies skipped.
* **HD canvas prototype** (`-Hd 2..4`, run.cfg `hd S`): the native machine's GPU also keeps its two display buffers at S times the resolution; polygons are rasterised on the
  finer grid (per-pixel barycentric interpolation), textured sprites are drawn directly on the HD grid, every other pixel write (tiles, lines, fills, uploads) becomes an S x S
  block. **Textures are sampled with an EPX (Scale2x) filter** (`hdfilter 1`, the default; applied twice at 4x; `hdfilter 0` = nearest): a texel is split into sub-pixels that take the
  colour of the neighbouring texel on their side when the two neighbours around that corner agree and the opposite pair does not, so sprite, UI-text and map-texture edges stay crisp
  and staircases are smoothed (`port/samples/native-frames/09-...` at 4x, `10-` / `11-` filter on / off at 2x). Shots get an `h<frame>.png` twin. Verified in the lockstep (8,300 frames
  to the first battle at 2x, 3,000 frames with the filter, RAM and VRAM identical -- the 1x path is unchanged; the filter's neighbour fetches do not mark VRAM as read). The art is
  still the PS1's 15-bit textures (no new detail), and the CPU rasteriser is ~5-20x slower at 2x-4x than at 1x: the real thing is a GPU renderer fed with the same command stream plus
  replacement art. Save states make HD iteration cheap: a render from a state takes 1-2 s instead of replaying 10,000 frames.
* **Save states** (`-SnapSave 'FRAME:name'`, `-SnapLoad name`, F1-F8 in play mode): the snapshot is the program's writable image (.data/.bss: both machines' HLE, GPU, GTE, interpreter and
  the driver), PS1 RAM, scratchpad, the thread-stack window and the coverage thunks, all-zero 4 KiB pages stored as a flag: 0.8-11 MB per state, valid only for the exact program
  build that wrote it (the header records the image layout). The game is always suspended at a frame boundary when a state is written or read, so its registers are already on its
  own stack inside the window; after a load the run configuration is read again. `tools/statediff.py A.state B.state` lists where two states differ, with symbol names.
* **Determinism** (`-DetCheck 'FRAME:M'`, `-HashEvery N`): (1) in one process, save the state after FRAME, run M frames recording a hash of everything the game can see (RAM,
  scratchpad, stacks, VRAM), load the state, run the same M frames again: identical at every frame (200 frames of the title, 1,700 frames of the first battle). (2) Two
  independent processes with the same inputs print identical state hashes (22 samples over 11,000 frames, title -> battle with random play). (1) proves the snapshot holds all
  the state, (2) proves the game does not depend on the process. What broke (2) at first, found with `statediff.py`: the coroutine switch left the driver's stack pointer (a host
  address, different in every process) as a dead argument slot on the game's stack -- now an argument-free `ls_yield`. This is what lockstep multiplayer needs: the same
  binary + the same inputs = the same game, hashes for desync detection, snapshots for rollback and join-in-progress.
* Two driver bugs found on the way: the coverage-thunk mapping (a fixed address) ended up *inside* the growing `.bss`, silently replacing part of the native GPU struct
  (mappings now use `MAP_FIXED_NOREPLACE`, `_start` checks the image end), and log lines printed before the run configuration was read went to stdout, which in play mode carries the frames.

Regression after all of this: 16 of 16 seeds x 12,000 frames identical with RAM and VRAM compared.

**Result 6: two players, one game** (2026-09-30 night; `lockstep.c` "Two seats", `netplay.py`, `netplay_test.py`).

* *How the game decides who plays:* a unit is played by a human only if `team_flags & BATTLE_TEAM_FLAG_PLAYER_CONTROLLED` (0x08; `battle_stats_t` +0x05, copied to `battle_unit_misc_data_t`
  +0x13d); every other unit is played by the AI (`battle_menu_get_unit_action_menu_id`, `battle_menu_dispatch_idle_action_menu`, `battle_ai_*`), `auto_battle_setting` turns a player unit
  over to the AI. In the first battle only Ramza (unit slot 0) is player-controlled; Delita, Algus and the other allies are AI units. `g_battle_turn_unit_id` names the unit whose turn it is.
* *Seats* (`seats 2`, `seat2units MASK`, `hotseat MASK`; `-Seats/-Seat2Units/-Hotseat/-Pad2Seed` in `lockstep.ps1`): the driver picks the controller the game sees, each frame, from the game
  state -- in battle, while the turn unit is player-controlled and its slot is in `seat2units`, controller 2, otherwise controller 1 -- and `hotseat` sets the player-control flag on more
  unit slots every frame (both structures). With the allies flagged, the game opens the command menu on their turns and waits for controller 2 when seat 2 owns them.
  Verified in the lockstep (original machine code alongside, RAM and VRAM compared): 14,000 frames of the first battle with slots 1-4 flagged, slot 2 owned by seat 2, random play on both controllers.
* *Netplay* (input-delay lockstep): `netplay.py` exchanges one 9-byte record per message over TCP ('P' = my controller for frame f + delay, 'H' = my state hash of frame f), `play.py --host PORT` /
  `--join HOST:PORT` are the two windows, `netplay_test.py` the headless test (two containers, loopback). Only two bytes of controller per frame and a hash every 60 frames cross the wire.
  Result: 14,000 frames with player 2 playing slot 2's turns (controller hand-over visible in the logs, `[frame 8658] controller 2 plays now`), 233 state-hash samples identical on both
  instances; the same with 15 ms of random jitter per frame (6,000 frames, 100 samples). The game needs no changes for this: same binary + same inputs = same game (Result 5).
  **Joining a game in progress** (`play.py --invite PORT` for the host, `--join HOST:PORT` for the guest): the host plays alone; when a guest connects the host's viewer asks the driver for a
  whole-machine state, sends it with the two-player configuration (`C` / `S` blobs on the same link), rewrites the session's run.cfg (a mounted directory the driver rereads whenever
  a state is loaded) and loads the same state itself; the guest boots its game, loads the received state and both continue from identical memory with the frame counters aligned (a loaded
  state also restores the frame counter). Headless test (`play.py --test-frames`, two processes, real Tk code path with the window hidden): the guest joined at game frame ~1,800,
  2,400 frames later 40 state-hash comparisons had matched and none had differed; when the guest leaves the host stops at that frame (lockstep cannot go on without the other controller).
* *Not covered:* a real network (latency and loss beyond TCP, NAT traversal), recovering from a desync (the state transfer used for joining would do it), more than two players, the world map and menus
  (controller 1 only), states / pause (saving or loading on one side would desync, so the viewer disables them in netplay), and **enemy units as player units** (PvP hot seat): with the enemies
  flagged the game runs code retail never runs -- the equipment screen of a non-party unit -- where the native build and the original differ in how they handle the garbage, and later the native
  build crashed (frame 23,891 of the test). Co-op with the AI allies is the natural first mode; PvP needs the enemy units to carry real party data.
* *Scenario generators* (reach code the first battle never runs; all are run.cfg lines or `-PokeWhen` strings, applied to both machines of a lockstep):
  `g_battle_entd_selection_mode=0:g_battle_entd_selection_mode=3:repeat` makes the first battle draw a random encounter from ENTD sets 1-59 (a Chocobo on the player's side, other jobs ...);
  `hotseat MASK` (player-control flag on more units), `caster MASK SEED` (a magic skillset, 999 MP and every ability learned for the non-monster units in MASK),
  `autobattle MASK` (the units play by the AI: battles without input), `effectmap SEED` (every ability plays one of the documented effect files), `traceabil 1` (log each ability a unit
  starts to use), `cdtrace 1` (log CD reads with frame and sector), `snapsave` / `snapload` (start every seed from a battle-start state), `soak.ps1 -ExtraCfg 'line;line'` (per-seed lines, `{seed}`).
  Result: 24 of 24 random-encounter seeds x 14,000 frames identical (RAM + VRAM) after two comparison rules were relaxed -- a function the retail code runs on the MAIN stack
  (`battle_thread_call_on_main_stack`) runs on the worker's own stack natively, so a stack address in an HLE argument is of another class; and the stack spills of WLDCORE (which runs on a stack IN the
  scratchpad) stay in the original's scratchpad after the module is evicted (now remembered as stale until both machines agree on the word again). Effect overlays: only the 110 effect files
  that the decomp documents have native code, and the AI casters (`caster` + `autobattle`) loaded effect data from other files (CD reads at LBA 7000 / 61809 in the trace: no match in the module table) without a
  crash, but also without reaching any of the 110 native effect files. Remapping every ability
  to documented effect files (`effectmap`) made the native build crash at the first effect (a jump into data at `g_battle_ai_unit_crystal_treasure_status+78`; not analysed yet: probably a
  function of an undocumented file, or an effect that needs state the remapped ability does not provide). Effect coverage therefore needs a function-level fuzz of the effect files or more
  decomp progress, not more scenarios.

* *Persistent shared world (MMO-ish):* nothing here supports it. The game is one save (world map, party, story flags, battles as instances); lockstep shares ONE such game between 2-4 players.
  A shared world would be a server-side reimplementation of the world state around battle instances.

**Result 7: sound** (2026-10-02; `hle/spu.c`, `audio_out.py`, run.cfg `audio 1` / `audiodump PATH`, `play.py --mute`).

* *The music driver was not running at all:* the game's sound driver (Suzuki) advances its sequencer and sound effects from a **root-counter-2 event handler** (`SuzukiSPUInitialiser`:
  `OpenEvent(0xf2000002, ..., main_sound_root_counter_2_handler)`, `SetRCnt(.., 0x44e8, ..)` = 240 Hz), which the HLE never called. It now calls that handler four times per vertical
  blank on BOTH machines of the lockstep. The whole sequencer (the `main_smd_*` / `main_sound_*` functions, ~1,000 of them) therefore runs natively and is verified against the original
  at every frame, including every SDK call it makes into the SPU (voice volume / pitch / address / ADSR, key on / off, pitch-LFO / noise / reverb voice masks): 3,000 frames identical.
  (A harness fix was needed: the original does not trace SDK calls made inside a re-executed `VSync(n)`; calls made from the callbacks are now traced either way.)
* *The sound chip* (`spu.c`, native machine only, never feeding anything back to the game): the libspu calls update a register model exactly as the decomp's libspu sources do
  (`SpuSetVoiceVolume` masks, `SpuSetVoiceARAttr` / `SRAttr` / `RRAttr` bit layouts, `SpuSetKey`, transfers into the 512 KiB sound RAM with `SpuWrite`); 24 voices decode ADPCM
  (the five filters, loop / end / repeat flags), play at `pitch / 0x1000` with linear interpolation, run the hardware-style ADSR (linear / exponential, the rate counters) and mix at
  44.1 kHz stereo with per-voice and main volume; 735 samples per frame. The **reverb unit** is modelled too (the 22 kHz comb / all-pass network of the hardware, fed with the game's own
  preset tables `_spu_rev_param` / `_spu_rev_startaddr` when `SpuSetReverbModeParam` selects a mode; depth from `SpuSetReverbDepth`; a reverb tail is visible in the dump after the music stops;
  the echo / delay modes' delay-feedback tuning is not applied). **Not modelled:** noise, pitch modulation, volume sweeps, CD-XA audio.
* *Result:* a 200-second native run from the title through the opening and into the first battle keys on 4,119 notes; the level follows the music (RMS 400-10,000, tonal
  zero-crossing rates, a few clipped bass peaks). **Nobody has listened to it yet** -- play.py streams the audio to the Windows waveOut API (ctypes, no packages;
  `audio_selftest.py` checks the device with silence) and `audiodump` + `tools/wavstat.py` / `wavzcr.py` write and measure raw dumps.

**Comparison rules** (documented, not bugs): the kernel area below 0x8000f800; `g_psyq_crt_constructors_ran` and the `g_psyq_*_saved_ra` words; the thread records' register
save areas and stacks; the first 23 words of the scratchpad (the hand-assembled blitters and 64-bit routines park registers and loop temporaries there); a word that holds a
stack address or a GPU tag pointing into a stack counts as equal when both machines' values are in the same stack (class = main / battle thread n / world thread n), and a word
whose stack-address bytes were only partly overwritten keeps "equal" as long as the surviving residue bytes are the ones that differed before (`residue_equal`); dead garbage
that retail leaves in unused struct padding / unassigned locals and natively is zero: `effect_list_node_t._padding_15`, the upper half of the turn banner's
`projected_display_value` (and its HELPMENU copy); HLE calls whose arguments are addresses in the same thread stack are the same call; VRAM pixels nobody has read.

**Retail-ABI accidents the lockstep found** (each is a place where the decomp's C relies on what the MIPS compiler happened to do; all fixed by a reviewed, exact-match patch in
`native/native_patches.py` (applied to the portified copy only) or by a generic pass in `tools/portify.py`; `git diff` of the repository stays empty):

| Class | Example | Fix |
|---|---|---|
| stale-register arguments (`((void (*)(void))f)()` of a function that takes arguments) | `main_party_save_unit`, `battle_effect_init_data`, `battle_unit_set_target_animation_from_attack_type`, the opcode-handler tables of BUNIT/EQUIP | patch: pass the value the register holds (established by replaying the original) |
| a `void` callee's leftover `$v0` is consumed | `equip_menu_update_*_selection_and_mark_change`, `world_gfx_bind_data_pointer` | patch: make the value explicit |
| narrow return value consumed as 32 bits (`s16`/`u8` getter cast to `s32`): retail callees return already extended values, x86 leaves the upper bits undefined | `world_input_get_tutorial_buttons` (input word 0x8000 vs 0xffff8000), 18 more | portify pass: the cast call becomes a real call with a widening cast (19 sites) |
| adjacent locals / argument slots used as an array | `corner0..3`, `cursor_polys[2]`+`shadow_polys[2]`, `wldcore_window_build_render_record_image` reading `position` and `dimensions` as 8 contiguous bytes | patch: explicit arrays / field copies |
| address of a pointer passed where the pointer was meant | `SetSemiTrans(&frame, 1)` in both scroll-list threads (writes one byte past the slot: padding on MIPS, a live local natively: row_offset became 0x02000000) | patch: drop the call (provably a no-op in retail) |
| index one past a 4-byte array | `battle_ai_choose_wait_facing` reads `viable_directions[4]` when the unit stands on the target: unused stack padding in retail | patch: explicit zero byte |
| uninitialised locals / stack padding | `world_menu_build_available_item_list` (`selected` when the current location is not in the list), `battle_menu_display_projected_action_effect` (`value`), `fallback_direction` | compare rule (dead value) or scenario fix |
| NULL-pointer reads of console low RAM | thread status indicators, `RotTrans(NULL)`, `battle_menu_build_unit_portrait_poly`, `world_formation_stage_selected_unit` (empty party) | source stand-ins; the NULL page logs the rest |
| code bytes read as data (the native code region holds x86 trampolines) | the BATTLE scroll list derives CLUT rows from a buffer `StoreImage` was supposed to fill (no VRAM: stale overlay code) | `StoreImage` returns real VRAM contents |
| cross-stack pointers in RAM | GPU tags linking to packets on a thread stack | comparison rule above |

Native transliterations of hand-assembled routines added on the way: `world_gs_sortpoly` (libgs `GsSortPoly`, undecompiled, registered through `yamlfuncs.EXTRA_FUNCTIONS`),
the four text blitters, the four map queuers.

Tooling for finding the cause of a divergence (flags of `lockstep.ps1`, see `port\README.md`): a watchdog reports a native frame that never reaches its next VSync with a backtrace;
`-Watch` prints game variables as they change; `-Replay N` records the original's *function-call sequence* for frame N (with arguments, callers, the scratchpad and watched words at
every entry and the original's writers of the first watched word), then runs the native game under live comparison and stops at the first call, argument, watched word or
scratchpad word that differs; `-Dump N` / `-DumpAround fn` print the calls with their callers; `-NatWatch` reports every store of the native game to a word (page protection plus
single step) with the storing function; the RAM report shows the words around the first difference; `-GpuWatch x,y` lists the SDK calls that wrote one VRAM pixel; `-PolyDump`,
`-TexDump`, `-SkipCmd` look at what the rasteriser is fed. Finding a divergence in a 60,000-frame run typically takes one `-Replay` and one `-Watch`.

Bugs this found (all fixed): the interpreter's BIOS function numbers for `strlen/bcopy/bzero/memcpy/memset/memcmp` were off by one against the game's own
veneers (`bcopy` was executing as `bzero`), which had silently made the function fuzz skip every function that reached them; `main_noop_800449ec`
(a `kind: blocked` stub) was missing from the native tables, so a call through its PS1 address executed MIPS code as x86; the native `main()` stored 0
where retail stores its `$sp` for the soft reset and had no way to restart the game loop (now a builtin `setjmp`/`longjmp` in `replacements\main_asm.c`); an
undecompiled libgs routine (`world_gs_sortpoly`) was executed as x86 (its MIPS bytes decoded as a call); and the retail-ABI accidents of the table above.

**Result 8 -- the equipment screen (2026-10-03).** A 32-seed random-encounter soak of 40,000 frames each (sound driver running) found 29 identical and three (seeds 1109, 1116, 1121)
diverging inside the EQUIP overlay (the screen that opens from the battle / world menu). Four native-only bugs, all retail-ABI accidents of the kinds in the table above, all
fixed by reviewed patches in `native_patches.py` (nothing in `fft_decomp` changed):

1. `equip_menu_load_images_and_reset_lists` stores a 16x1 VRAM rectangle (32 bytes) into `u16 buf1[4]`; retail's frame has `buf2[12]` right above it so the store is absorbed, a native
   frame has the saved registers there. The VRAM palette data landed in the saved `ebx`, and `equip_menu_init_screen` stored it as the unit id (`g_equip_unit_status_panel_flags`
   = 0x4a303527). Found by tracking the callee-saved registers at every call entry of the replayed frame (new debugging aid: `lockstep.ps1 -Cflags '-DREGTRACK_LO=a -DREGTRACK_HI=b'`).
2. `equip_gfx_build_item_graphic_descriptor` calls `battle_get_item_graphic_data(&graphic)` without the item id: retail passes its own `$a1` through. Now a real parameter.
3. `equip_thread_start_if_idle` tests `battle_thread_is_running()` with no thread id (retail: still in `$a0`); the stale stack word was once 0xf83508b4 (native crash at frame ~24,185).
4. `equip_entrypoint` closes with `main_unit_refresh_stats_and_statuses()` without its unit (retail: `stats` still in `$a0`, disassembly 0x801bf9a8..0x801bf9f4); the wrong
   memory was refreshed (`g_battle_unit_misc_data+0x148`, NULL-page reads).

Also changed: the NULL-safe portrait patch uses a statement expression so the call sequence of `battle_unit_get_stats_from_battle_id` stays that of the original (a replay compares
call sequences). After the fixes: **32 of 32 seeds identical over 40,000 frames** (1,254 s on 16 cores; function coverage union 2,876 of 5,318, event overlays 510 of 777);
NULL-page accesses left: `bcopy` in `equip_entrypoint` (`g_equip_unit_data[0]` is NULL for a moment in two seeds; reads zero here, a Windows build needs a stand-in).
Not yet analysed: `world_menu_resize_parent_entry_to_digits` calls `world_text_count_decimal_digits()` without its argument (retail leaves a stale `$a0` from the caller).

**More long soaks (2026-10-03).** World-map seeds (battle-end cheat, no story events) 32 of 32 identical over 40,000 frames, no NULL-page access. AI-caster + autobattle seeds
(`caster 0x1f {seed};autobattle 0x1f`, random encounters) 31 of 32 over 30,000 frames; the one divergence (seed 3027) was **undefined behaviour exploited by the modern compiler**:
`battle_menu_display_projected_action_effect` reads `action->attack_accuracy` and only afterwards tests `action == 0`; gcc deletes that test (`-fdelete-null-pointer-checks`), GCC 2.6.3 does
not. All native builds now compile with `-fno-delete-null-pointer-checks` (a policy, like `-fwrapv`); seed 3027 passes and the 32-seed random-battle regression stays 32 of 32 (30,000 frames).
NULL-page reads still logged (retail reads zeros from console RAM): that function (action NULL), `battle_ai_load_ability_entry` (unit index from a bad packed id), `bcopy` in `equip_entrypoint`.

**New upstream base (2026-10-03).** The decomp moved on (`adamrt/fft_decomp` 2b09e33: its maintainer fixed many of the same retail-register arguments and documented the same overflows, independently). The native port now builds from our `remove-unneeded-pins-and-barriers` branch rebased onto it (`mktree.ps1 -Rev pub/remove-unneeded-pins-and-barriers`, fft_decomp checked out detached at that commit). 14 of the 48 patches became redundant and were dropped; 2 had to be rewritten for upstream's new text (`equip_menu_update_{vertical,horizontal}_selection_and_mark_change`: upstream gave the wrapper real parameters but the inner call still drops the `input_mask` that retail passes through `$a2`), `portify.py` can now list every patch that no longer applies (`PATCH_KEEP_GOING=1`). Result: 32 of 32 random-encounter seeds identical over 30,000 frames (first attempt without the two rewritten patches: 15 of 16, the failure was exactly that missing argument).

**Result 9 -- a GPU renderer (2026-10-03).** The CPU software rasteriser (`hle/gpu.c`, about 15 fps at 2x HD) is now optional: with run.cfg `gltrace 1` the container records every primitive the software GPU rasterises as vertices with absolute VRAM coordinates, plus the VRAM transfers (uploads with their pixels, copies, fills) and the display settings, and sends that command trace per frame (`'G','L'` packet) instead of pixels; `port/native/gl_renderer.py` replays it with OpenGL (moderngl): VRAM as a 16-bit integer texture, the framebuffers as an RGBA8 texture at S times the resolution, a fragment shader that decodes 4/8/15-bit texels, CLUT, texture window, modulation and 5-bit colour, and per-pixel dual-source blending for the four semi-transparency modes (an earlier two-pass version drew a full-screen tint over glyph texels that came before it -- primitive order matters). The software model stays the oracle: `gl_verify.py` runs the game with `gltrace 2` (trace plus the software picture of each frame), replays on the graphics card at 1x and compares per pixel (`--vram-check --fb-check` also compare VRAM mirrors and the whole framebuffer). Results (RTX 3070): title / intro / dialogue 12,000 frames, two random-battle seeds x 14,000 frames and a world-map run x 14,000 frames: 100.0% of frames have < 0.1% differing pixels (only 1-pixel-wide rain triangles can cover other pixels: the two rasterisers' edge rules), GL replay 0.5-1.5 ms per frame with 500+ triangles. Not reproduced: a region the game renders into and then samples as a texture (the mirror check found none in these runs), dithering. `gl_selftest.py` checks that an OpenGL 3.3 context opens. Needs `port\build\venv` (moderngl, glfw, numpy, pillow). The window / input viewer on top of it is the next step (`play_gl.py`).

**Result 10 -- platform-layer fidelity: libgpu's own state (2026-10-03).** The owner pressed Enter through New Game and the name-entry screen flashed an empty window (no text). Replaying the logged key presses (`play_gl.py` writes every controller change to `port/build/native/ls/last_input.txt`; `--replay` plays it back headless) showed the same thing on the ORIGINAL machine code (lockstep identical), so it was the platform layer, not the native build. Two SDK calls that the layer takes over (`PutDrawEnv`/`PutDispEnv`, `ResetGraph`) skipped state that libgpu's own code, which the game runs natively, reads later: (1) `GetDrawEnv` returns the copy that the real `PutDrawEnv` keeps in `g_psyq_gpu_environment.draw`, and the name-entry loop uses it to pick the framebuffer for its keyboard bitmap -- stale copy, bitmap into the displayed buffer, flashing; (2) `get_cs`/`get_ce` clamp every `SetDrawArea` to `g_psyq_gpu_vram_width/height`, which `ResetGraph` sets to 1024 x 512 -- zero, so every menu draw-area packet became (1023,1023)-(1023,1023) and the text was clipped away. `hle/gpu.c` now stores the defined fields of both environments (the DR_ENV packet area of the game's own struct is uninitialised stack and is stored as zeros, otherwise the two lockstep machines differ) and initialises the libgpu state in `ResetGraph(0/3)`. Verified: the owner's session replayed on both machines identical (1,500 frames), 8/8 battle and 8/8 world-map seeds x 14,000 frames identical, the screen now shows the keyboard, the name and the Check OK/NO menu. Lesson / audit item: every SDK function the layer replaces must also keep the library variables that the game's own code reads afterwards (see `.agents/streams/backlog.md`: HLE state audit).

**Result 11 -- the GTE's colour / lighting commands (2026-10-03).** The terrain of the opening scene (the Gafgarion / Agrias dialogue) was pure red and faded in and out. The replay of the owner's session (`play_gl.py --replay`, `--trace-at`, `--vram-at`) showed every map polygon with vertex colour (255, 0, 0): the map-drawing routines of the BATTLE overlay compute each polygon's light with the GTE commands NCS / NCT, and the software GTE had `default: break; /* colour/lighting commands ... unimplemented on purpose */` -- the commands did nothing, so the colour FIFO kept a stale value. Both lockstep machines share that GTE, which is why the soaks never saw it (and why the old green/magenta battle-map tint is probably the same bug). `gte/gte.c` now implements NCS NCT NCCS NCCT NCDS NCDT CC CDP DCPL DPCS DPCT INTPL GPL from the public hardware description (MVMVA-style matrix helper, saturating colour FIFO, depth-cue interpolation with the far colour); `gte/tests/test_gte.c` checks them against an independent plain-C reference (4,827,863 checks, 0 failures). Result: the scene renders with natural stone, wood and glass colours and only the carpet red; 8/8 battle and 8/8 world-map seeds x 14,000 frames and the owner's first session identical on both machines. Not yet re-checked after the fix: the old battle-map tint in battles, the other GTE commands' flags (only values are tested), and whether further hardware behaviour of the GTE is missing (e.g. the MVMVA far-colour quirk).

**Result 12 -- a second decomp base: the semantic-cleanup fork (2026-10-09).** [LuizORAS/fft_decomp-semantic](https://github.com/LuizORAS/fft_decomp-semantic) branch `semantic-cleanup` (123 commits on upstream 2b09e33: names, types, signedness, SDK barrier removal, a division audit in `QUIRKS.md`, and `world_gs_sortpoly` decompiled) builds natively with the same port code: `native_patches.py` entries may carry `done` (upstream fixed this place itself: skip) and `alt` (the same edit for a later spelling), and `replacements/world_asm.c` leaves out our `world_gs_sortpoly` when the decomp defines it (`build_run_lockstep.sh` passes `-DDECOMP_HAS_GS_SORTPOLY`; a weak symbol does not work, the archive member is then never pulled). On that tree: random play 8/8 x 12,000 and 16/16 x 30,000 frames, random encounters 16/16 x 30,000, world map 8/8 x 30,000, GPU renderer 6,000 frames 100% within tolerance; upstream's tree stays 8/8. Upstream remains the base of this project (it is the canonical decomp and what our fork tracks); the fork is a second, tested target. A replay of the owner's 2026-10-03 session also showed that the first battle does end (event 6, then the story and the save screen): the report of a battle without an end condition was not reproduced.

**Result 13 -- platform-layer audit, first pass: libspu's RAM state (2026-10-09).** `port/tools/hle_audit.py` lists, for every SDK function the platform layer replaces, what the real library code (the decomp's C plus the SDK functions it calls) writes that code still running natively reads back -- globals and output parameters. Of 220 replaced functions, 18 keep such state. libgpu's was fixed in Result 10. In libspu: `SpuInitMalloc` / `SpuMalloc` / `SpuFree` were replaced by no-ops, so the SPU heap was never built and every sound-RAM allocation returned 0. The reverb attributes, `_spu_rev_offsetaddr` and the key-on mask were never written, and `SpuGetVoiceEnvelopeAttr` left both outputs as stack garbage (the music driver's end-of-track opcode waits on `envx == 0`). Now the pure-RAM functions run as real code on both machines (`HLE_KEEP_NAMES` in `hle/hle.h`; 231 instead of 238 functions taken over). `hle.c` keeps libspu's RAM bookkeeping for the replaced setters exactly as `_SpuInit`, `SpuSetKey`, `SpuSetReverb`, `SpuSetReverbModeParam` and `SpuSetReverbDepth` do. The envelope query gets a deterministic stand-in from the key-on mask. Result: the heap holds one 0x78000-byte block at 0x1010, reverb mode 4 at 0xf204. Soaks pass 4/4 x 12,000 frames, and 200 s of the owner's session sound the same as before (the main banks use fixed addresses). Remaining replaced calls with an unset return value: `GetGraphType` / `GetVideoMode` (0 = an original-model NTSC console: correct), movie streaming (FMVs are skipped), and **the memory card**: the layer emulates a console with no card inserted, so the game cannot be saved.

**Result 14 -- a virtual memory card: the game saves and loads (2026-10-09).** The platform layer emulated a console with no memory card, so the game could not be saved (the owner's session ended on "Memory card is not properly inserted"). `hle/card.c` now models the card in slot 1 as a 128 KiB raw `.mcr` image (header, directory, 15 blocks, the format emulators use). It implements the BIOS card-file calls the game makes (`open` with `FCREAT` and a block count, `read`, `write`, `lseek` SET/CUR, `close`, `erase`, `firstfile`/`nextfile`, `format`), the low-level `_card_*` calls and the SwCARD/HwCARD events. Each lockstep machine owns a card started from the same image. run.cfg `memcard PATH` keeps the card in a host file, written after every frame that changed it and read again after a state load (loading an older state does not undo a save); `play.ps1` / `play_gl.py` use `port/build/states/memcard0.mcr`, headless tests use a throwaway card. A call trace (`cdtrace 1` also lists the card file calls) caught one wrong assumption: the game rewrites the 0x80-byte block at 0x100 with `lseek(0x100, SET)` + `lseek(0, CUR)`, so `SEEK_CUR` is relative. Result: replaying the owner's session, the game saves "File 01, Ramza, Squire Lv01, Magic City Gariland" as `BASCUS-94221FFTA` (a standard `SC` save block). Lockstep stays identical to the original code through the save (56,610 frames), Continue on the title screen loads it into the next battle, and soaks pass 4/4 + 4/4 x 12,000 frames.

**Result 15 -- replayable scenes and a texture filter on the GPU (2026-10-09).** *Scenes*: a scene (`port/scenes/NAME.json`) is the controller input from power-on to its last frame, its interesting range, the memory card it started with, and, once accepted, a hash of the picture at every checkpoint. It contains no game data and survives rebuilds, unlike save states, which only load into the build that wrote them. The GPU viewer records them: F9 starts and F9 ends a scene. The viewer now keeps the input history from power-on across F1-F8 state saves and loads (each slot gets a `slotN.input.json` next to it) and copies the memory card as it was at power-on. `port/tools/scene.py run NAME` replays a scene in lockstep against the original code and compares the checkpoint pictures; `accept` stores them; `from-input` turns a logged session into a scene. The first two, from the owner's 2026-10-03 session: `new-game-to-orbonne` (power-on to the end of the first battle, 31,000 frames, 22 checkpoints) and `orbonne-victory-and-save` (victory event, story scene, save to the memory card, 51,000 frames, 15 checkpoints). Both are identical to the original code at every frame. *Texture filter*: `play_gl.py --filter epx` (T toggles it) runs EPX / Scale2x on the game's texels in the fragment shader at 2x-4x. Each texel's four quarters take a neighbour's colour where two neighbours agree, so sprite outlines and text are rounded instead of blocky. The default stays the exact 1x texel, which `gl_verify.py` checks against the software GPU. Widescreen is not done: the game's own projection, culling and 2-D layout assume 320 pixels, so it needs game-side patches, not a renderer option.

**Result 16 -- effect files: what they are, and the `effectmap` cheat fixed (2026-10-09).** Of the 512 `EFFECT/E*.BIN` files, the 110 the decomp has modules for start with MIPS code; the other 402 start with an offset table: they are data (effect scripts the BATTLE engine interprets natively), so they need no decompiled code. In the game's own `s16 g_battle_effect_ability_ids[512]` (negative = no effect, bit 0x800 = item, low bits = the file number) 112 abilities use a code-type file, with no flag that sets them apart. The `effectmap` scenario wrote that table as 32-bit words: it remapped only every other ability and overwrote the 1 KiB after the table. That was the 'jump into data' crash of the earlier note. It now writes the 16-bit entries and keeps the flags. With it, AI casters load the code-type files as native modules (E259 in seeds 1 and 2), and the native game then crashes in `battle_effect_start_script_record` on a pointer read from the effect header (0xa300060e). Not yet known: whether the original code makes the same bad read at that point (a remapped ability given a file it was never meant to use), or whether it is a native-port bug that only code-type effects reach. Next step: let the casters use the 112 real abilities that own code-type effects instead of remapping.

**Result 17 -- a Windows build without Docker (2026-10-10).** The native game now also builds as a 32-bit Windows program (`port/native/win/build_win.py`: i686 MinGW-w64 GCC 12.4, the container's flags plus `-mno-ms-bitfields -mno-align-double -fpcc-struct-return` for the Linux i386 ABI). The driver's few Linux system calls are done with Win32 by `port/native/win/win_sys.c`. The program contains the R3000 oracle, so it was verified the same way: the owner's whole first session (31,000 frames) and a random-play seed (12,000 frames) are identical to the original machine code at every frame. In the GPU viewer it runs at about 260 fps without a virtual machine (`play.ps1 -Native`). Windows exposed six places where the native build had matched the original only by luck: functions that return nothing (or are `void`) while callers use the value, which on the PS1 is whatever `$v0` holds. They are fixed by reviewed patches that return the retail value read from the machine code, and Linux stays 4/4 seeds x 12,000 frames plus both scenes identical. Further findings: dead stack memory the game still reads (Win32 calls now run on a stack of their own, as Linux system calls do), page-zero reads (emulated in the exception handler), and a container-only build flag (`-DLOCKSTEP_THREAD_WINDOW`). Details: `WINDOWS-NATIVE.md`.

## 10. Open items

* Audio: the SPU (XA streams, ADPCM voices, reverb) is still only logged by the HLE; the movies (MDEC) are skipped.
* Effect overlays (110 files, 210 functions with yaml docs) are barely exercised: a scenario that makes units use many abilities (poke the skillsets), or a per-function
  fuzz of the EFFECT module set.
* The rest of the WLDCORE/WORLD surface (travel, shops, formation, tutorials) with deeper random play; the world soak starts from a poked state (no story events ran).
* Verify the GPU model against a reference (an emulator's output for the same frames) -- the frames look right, but bit-exactness with the hardware (dithering, exact
  edge rules) has not been checked; dithering is not modelled.
* The HD renderer proper: a GPU renderer fed with the same command stream (internal scale factor, filtered / replaced textures, widescreen); then the platform layer (window, input, audio)
  outside the test harness (a Windows build needs a toolchain that is not in the docker image: a download that needs your approval).
* A source-level division policy for non-x86 targets.
* Netplay prototype on top of the determinism result: two instances + an input relay (lockstep with input delay), co-op by routing the pad of the unit's owner, periodic state hashes
  for desync detection, a snapshot for joining a game in progress.
* Use save states to start scenarios directly (a battle turn, a shop) instead of replaying 11,000 frames: the route to effect-overlay coverage (poke abilities, one trial per state).
