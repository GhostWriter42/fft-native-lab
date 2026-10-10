# The Windows build (no Docker)

Status (2026-10-10): **works.** `port/build/win/fft_native.exe` is the native game built for Windows. It plays the owner's first session (power-on, title,
name entry, the opening, the whole first battle: 31,000 frames) **identical to the original machine code at every frame**: the program carries the
same R3000 interpreter as the container build and runs the original next to itself when asked (lockstep). In the GPU viewer it runs at about
260 frames per second, so the 60 fps game uses a fraction of one core and no virtual machine.

    powershell -File port\native\play.ps1 -Native -Hd 2          # play: the graphics card draws, no Docker

## What it is

* The game: the same portified decomp tree and the same reviewed native patches as the container build, compiled by an i686 MinGW-w64 GCC 12.4
  (WinLibs; `port/build/toolchain`, not in the repository) with the container's flags plus three that give the Linux i386 ABI:
  `-mno-ms-bitfields -mno-align-double` (struct layout) and `-fpcc-struct-return` (small structs returned in memory).
* The driver (`port/native/lockstep.c`): unchanged apart from a few `_WIN32` switches. Its Linux system calls go to `port/native/win/win_sys.c`,
  which does them with Win32: file I/O through the container's mount names (the environment variable `FFT_MOUNTS` maps `/disc`, `/states`, ... to
  Windows folders), `mmap` / `mprotect` as `VirtualAlloc` / `VirtualProtect` (the PS1 RAM at 0x80000000 needs a large-address-aware 32-bit process),
  signals as a vectored exception handler.
* `port/native/win/build_win.py`: the container's build steps (generated tables, compile, rename to `native_*`, per-overlay data copies, stubs, link
  with the symbol script). Objects are reused while their source text, the headers and the flags are unchanged: a full build takes about an hour, a
  patch to a few sources a few minutes.
* `play_gl.py --native-exe` / `play.ps1 -Native`: the GPU viewer starts the Windows program instead of `docker run`; the pipe protocol is the same.

## What the port had to fix (and what that fixed for Linux too)

Windows exposed places where the native build had matched the original only by luck. The values involved come from undefined behaviour in the
retail code, and Linux GCC happened to produce the retail values:

* **Functions that fall off their end or are declared `void` while callers use their value**; on the PS1 the caller gets whatever is in `$v0`,
  natively whatever is in `eax`. Fixed with reviewed patches that return the retail value explicitly (read from the machine code):
  the menu-script handler `world_menu_handle_window_command_with_scaled_clip`, `battle_move_has_reached_*` (3), `battle_move_calculate_walkto_pathing`,
  `battle_map_load_gns_and_move_find_items`, `battle_map_command_get_3d_object_state` / `..._get_texture_animation_active`.
  `port/tools/void_ret_scan.py` and `port/tools/void_table_scan.py` list the remaining candidates.
* **Dead stack memory**: the name-entry screen keeps its GPU packet buffers in its stack frame after it returns; a Linux system call does not touch
  the caller's stack, a Win32 call does. `win_sys.c` therefore runs every call on a stack of its own.
* **Page zero**: retail code reads console RAM at 0..0xffff through NULL pointers; Linux maps a zero page there, Windows cannot. `win_sys.c` emulates
  those loads (as 0), compares and tests in its exception handler and reports every site once (each deserves a patch).
* **Build flags the container passes** (`-DLOCKSTEP_THREAD_WINDOW`: the game's thread stacks in the mapped window, which PS1-style 24-bit GPU links need).

## Not done yet

* The CPU viewer (`play.py`) and the two-player join of a game in progress still use the container.
* The Windows program needs the extracted disc files and the disc image as the container does (`HOW-TO-PLAY.md`).
* Remaining return-hazard functions (`battle_script_filter_unit_id_by_mode`, `battle_target_set_weapon_attack_panels`, `battle_action_can_unit_react_1`):
  their retail values are still to be derived; until then the Windows and Linux builds may differ where they matter.
