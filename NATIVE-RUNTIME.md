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

Progress of the run (30 trials per function): see the table at the end of this file. Method notes worth keeping:
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
native game and the original code can be run frame by frame from the same boot and their RAM compared at every `VSync` � a whole-program
differential test, and the natural first milestone of the native runtime ("boots headless to the opening overlay"). The native side already has the
pieces it needs: the RAM image, trampolines, coroutine threads, the software GTE.

## 9. Open items

* Overlay switching in the native runtime (per-overlay symbol tables, trampolines, data image reload), and the same fuzz for WORLD / WLDCORE /
  OPEN / EVENT / EFFECT (each module needs its overlay image loaded next to SCUS).
* HLE of the SDK hardware layer; a null renderer; boot to title screen with scripted input.
* The four GTE map-queue routines and the two blitters (or their replacement by the HD renderer).
* A source-level division policy for non-x86 targets; the thread-context snapshot for rollback.
