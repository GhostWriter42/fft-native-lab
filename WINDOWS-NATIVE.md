# A Windows-native build (no Docker): plan

Status (2026-10-09): **not started**. The native game runs only inside the Linux toolchain container today. This note says what a Windows build needs, so the work can be
picked up in one piece.

## What is portable already

* The game itself: the decomp's C, as portified (`port/tools/portify.py` + the 34 reviewed patches), is plain C with no OS calls.
* The platform layer (`port/native/hle/*.c`: SDK calls, software GPU, SPU model, memory card) and the software GTE: freestanding C.
* The GPU viewer (`play_gl.py`, OpenGL) and the sound output already run on Windows. They talk to the game over a pipe (GPU command trace + audio out, controllers in),
  so a Windows build of the game can keep that interface unchanged.

## What is Linux-specific

1. **The driver** (`port/native/lockstep.c`): system calls through `int $0x80` (49 call sites: file I/O, `mmap` at fixed addresses, signals, `mprotect`), the start-up code,
   and the comparison with the R3000 interpreter. A Windows build needs only the *play* half: boot, frame loop, pipe protocol, save states, memory card. No oracle.
2. **The address-space layout**: the PS1 RAM is mapped at its own addresses (0x80000000..0x801fffff), the scratchpad at 0x1f800000, the thread and main stacks at
   0x80400000..0x80a00000, the trampolines at 0x10000000. A 32-bit process on 64-bit Windows linked with `/LARGEADDRESSAWARE` has a 4 GiB address space, so
   `VirtualAlloc(fixed address, MEM_RESERVE | MEM_COMMIT)` can place all of these. To be verified first: nothing (ASLR images, the heap, the stack) already occupies them.
3. **Page zero**: retail code reads console RAM at addresses 0..0xffff (through NULL pointers). Linux maps a zero page there (`--cap-add SYS_RAWIO`); Windows cannot.
   Each such read needs a reviewed source patch. The lockstep's NULL-page report lists the sites (currently `battle_map_calculate_slope_height+89`).
4. **Thread switching** (`replacements/battle_thread.c`, `world_thread.c`): hand-written stack switching in x86 assembly (cdecl, no OS calls). It should assemble unchanged with
   an i686 GNU toolchain, but needs testing.
5. **Toolchain**: an i686 MinGW-w64 GCC (the same compiler family and flags as the container: `-m32 -O1 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks ...`).
   This is a download (approved class: project tooling).

## Order of work

1. Install i686 MinGW-w64; compile the portified tree + platform layer into objects on Windows (no linking yet). This shows compiler differences early.
2. Write `port/native/win_main.c`: map the fixed regions, load the game image, run the frame loop with the pipe protocol of `play 2` / `gltrace 1`.
3. Patch the page-zero reads (soak in the container first with page zero unmapped to find them all: a run.cfg switch that skips the `SYS_RAWIO` mapping).
4. Point `play_gl.py` at the Windows executable instead of `docker run` (`--native-exe PATH`).
5. Check: the scenes (`port/tools/scene.py run ... --native`) give the same pictures on Windows as in the container.
