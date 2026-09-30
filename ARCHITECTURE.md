# How the game is put together (read from the decomp, 2026-09-30)

Everything below is taken from the source in `fft_decomp` (function names are the repo's). Items marked *(inferred)* are
conclusions, not read directly.

## Boot and timing

`main_boot_run_startup` (`src/main/`): clear the heap allocator table -> `ResetCallback` -> zero the play-time counters ->
install callbacks: `VSyncCallback(main_system_handle_vsync_callback)`, `DrawSyncCallback(… empty …)`, CD ready/read
callbacks -> `ResetGraph`, `PadInit`, `SpuInit` -> `main_gfx_reset_display(256, 240, 512, …)` -> logos -> memory-card
events -> generic SFX -> `srand(1)` -> fade out.

**The VSync handler does two things every vertical blank (60 Hz):** it calls `rand()` (the RNG is advanced *every frame*,
whether or not anything uses it) and it advances the play-time counters (`frames` -> `seconds` -> `minutes` -> `hours`,
capped at 1000 h) and `g_main_system_session_frames`. Consequences: the RNG state depends on the number of vertical blanks
that elapsed, **including load times**; play time is frame-counted, not wall-clock.

## The top-level loop (`main_system_run_game_loop`)

An endless `for (;;)`:
1. `main_item_init_new_game_inventory()`; on a fresh start `main_boot_reset_game_state()`.
2. **OPEN overlay** (title/opening): `main_overlay_exec_open_bin_main_loop(mode)` — loads `OPEN.BIN` (LBA `0x14ff0`,
   `0x36800` bytes) into the low overlay address and runs `open_system_run_main_loop`. Its result decides whether to go
   straight to a battle.
3. inner `do … while (game_flow_state != 3)`: **WORLD** — `main_overlay_open_world_and_wldcore(1)` loads `WLDCORE.BIN`
   (LBA `0x14849`, `0xDC` sectors) into the low overlay address and `WORLD.BIN` (LBA `0x14925`, `0x1E0` sectors) into the
   world overlay address, then runs `wldcore_entrypoint()`; then **BATTLE** — `main_overlay_exec_battle_bin()` loads
   `BATTLE.BIN` (LBA `1000`) into the low overlay address and `battle_state_run_game_loop()` runs the battle.
4. `game_flow_state == 2` -> reset the game; `== 3` -> ending sequence (`OPEN` ending overlay, a final battle-engine scene).

So the game is a **sequence of overlays that share addresses**: OPEN, WLDCORE and BATTLE all load at `0x80067000`; WORLD
sits at `0x800e0000`; EVENT/EFFECT overlays load above BATTLE (`0x801bf000…`). The resident executable is
`SCUS_942.21` at `0x80010000`.

## The battle loop (`battle_state_run_game_loop`)

Init: render buffers, render state, effect system. Then, **once per rendered frame**:
`battle_state_update_controller_input()` -> `battle_gfx_init_render_frame()` -> `SetGeomScreen(0x200)` ->
`main_gfx_swap_and_clear_otag()` -> camera update from game state -> `battle_camera_update_matrices` ->
`battle_map_draw_mesh_and_weather` -> `battle_effect_update_cycle()` -> a `switch (g_battle_game_state)` state machine
(`FREE_CURSOR`, `FREE_CURSOR_HELP`, `HIGHLIGHT_UNITS`, `OPEN_ACTION_MENUS`, `IDLING_ACTION_MENUS`, `MENU_TO_TARGETING`, …,
each with a `battle_state_handle_*_state` function). Deployment (`battle_state_run_deployment`) runs first.

The **only input poll is `battle_state_update_controller_input()`, once per frame** — the natural injection point for
synchronised network input.

## Cooperative threads (`include/fft/thread.h`, `battle_thread_*` / `world_thread_*` twins)

A table of **16 slots x 0x400 bytes** in RAM (`g_battle_threads` / `g_world_threads`). `battle_thread_start(id, fn)` fills a
slot: saved global pointer, `stack_pointer = frame_pointer = slot + 0x3f0`, `code_pointer = fn`, `is_running = 1`,
`task_id = 0`, seven per-task words. The **only switch point is `battle_thread_yield()`**; `battle_thread_wait_frames(n)` is n
yields (so one yield ≈ one frame). Threads run menus, camera moves, map lighting/darkness, message text, unit moves
and event-script actors. Task ids (resume/wait/stop handshake for dialogue text, camera, map light/darkness, event
block) are enumerated in `thread.h`. The context switch itself is hand-written (`main_restore_game_loop_stack_pointer.s`
is the single `.s` file in the repo).

## Event scripts

`battle_script_execute_event` / `world_script_execute_event` are twin bytecode interpreters (same opcode table, kept in sync);
script variables live in `g_battle_script_variables`. Event-script randomness uses its own RNG, seeded from `VSync(-1)`.

## What this means for a native port *(inferred)*

* **Frame-driven and single-input-point = lockstep-friendly.** One frame of simulation per iteration, inputs read once
  per frame, threads yielding per frame. A host can run N frames per sync point; peers only need identical inputs and RNG.
* **One native code module per overlay.** Overlays share addresses and their data symbols overlap, so the native build needs a
  separate symbol map per overlay (`gen_symbols.py` currently merges `main`+`battle` only), a data-image reload on
  each overlay switch (as the PS1 does from disc), and an entry table (`open_system_run_main_loop`, `wldcore_entrypoint`,
  `battle_bin_entrypoint`).
* **Threads as RAM-resident coroutines.** Implement `*_thread_yield` as a small x86 assembly switch that saves callee-saved
  registers on the thread's own stack and swaps `esp`, with the stacks *inside* the snapshotted RAM region (x86 frames are
  bigger than 0x3f0, so enlarge the slots in an extension region). Then a RAM snapshot captures every coroutine and
  rollback is exact.
* **Make the RNG explicit.** Replace the BIOS `rand()` with a native LCG whose state lives in RAM, split cosmetic from
  gameplay calls, and either tie the per-vblank advance to the simulation frame counter or snapshot/broadcast the RNG at
  every sync point (see `PORT-FEASIBILITY.md`).
* **Time is frames, not seconds.** Play time, animation and threads all count vblanks; a native main loop should run the
  simulation at a fixed step and render at any rate (which also gives high-refresh HD output without changing gameplay).
