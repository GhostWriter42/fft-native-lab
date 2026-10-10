# Playing the native build (prototype)

What this is: the decomp's C code compiled with a modern compiler, running next to nothing else but a small platform layer (software GPU, software sound chip, CD reader).
It is **not an emulator** and it needs your own disc image in `game\` (already there). It runs inside the Docker toolchain container; a small Python window shows
the picture, plays the sound and sends your keyboard back. Windows, Docker Desktop running, Python with Pillow and tkinter (already present on this machine).

## The launcher

Double-click **`Play FFT.bat`** in the project folder. A small window lets you choose:

* the program: the Windows build (no Docker) or the Docker build;
* the picture: resolution 1x-4x, texture filter, smooth window scaling, full screen;
* the game: fast effects (ability animations at 60 fps instead of the original 15-30), no sound, two players on one PC (the second gamepad);
* where to start: power-on, or one of the saved states (slot 1-8, with the date and what it is; a state saved by an older build of the program
  is marked, it will not load);

and has buttons to rebuild the Windows program and to remake the three test states. Your choices are remembered (`port\build\launcher.json`).
The game's messages appear in a console window next to it.

## One player

    powershell -File "C:\Storage\Build\FFT Mod\port\native\play.ps1"

The first run compiles the game (a few minutes). Options: `-Hd 2` / `-Hd 4` (3D drawn at 2x / 4x resolution and the textures edge-smoothed; software rendering, so slow:
roughly 15 frames per second at 2x), `-WorldCheat` (skip the first battle, go to the world map), `-Verify` (also run the original machine code beside it and compare every frame --
the game stops with a report if they ever differ), `-Build` (recompile).

Keys: arrows = d-pad, **Z** = Cross (cancel), **X** = Circle (confirm), A = Square, S = Triangle (menu), Q/W = L1/R1, E/R = L2/R2, **Enter** = Start, Backspace = Select,
P = pause, Tab (held) = fast forward, F12 = screenshot (`port\build\shots`), **F1-F4 save the whole machine state, F5-F8 load it** (save anywhere, also in battle; files in `port\build\states`).
On the title screen press Enter, then X a few times; the intro movies are skipped.

**Saving in the game** works as on the console: a memory card is in slot 1, kept in `port\build\states\memcard0.mcr` (a standard 128 KiB raw card image, the format
emulators use). The game's own Save and Continue use it; the F1-F8 machine states do not change it (loading an older state does not undo a save). Slot 2 is empty.

Sound: the music and effects play through Windows audio (`--mute` with `play.py` to switch off). It has not been listened to by the author of this build: if it sounds wrong, the
numbers in `NATIVE-RUNTIME.md` (Result 7) say what is and is not modelled.

## Without Docker (Windows build)

    powershell -File "C:\Storage\Build\FFT Mod\port\native\play.ps1" -Native -Hd 2

runs the Windows build of the game (`port\build\win\fft_native.exe`) in the graphics-card viewer: no Docker Desktop, no virtual machine. The first
start builds it (about an hour; it needs the i686 GCC unpacked in `port\build\toolchain`, see `WINDOWS-NATIVE.md`); `-Build` rebuilds it after
changes. Everything else (keys, gamepads, saving, `-Filter`, `-Two`, `-HostPort` / `-Join`) works the same.

## Two players (co-op prototype)

Each player runs their own copy; only controller values cross the network (see `port\native\netplay.py`). Player 1 plays the player-controlled units and the menus,
player 2 plays the turns of some of the AI allies (`-Seat2 MASK`, default units 1, 3, 4 of the first battle; `-Hotseat MASK` says which units become player-controlled).

    powershell -File "C:\Storage\Build\FFT Mod\port\native\play.ps1" -Invite 7777

starts playing alone and takes the other player into the game in progress when they connect;

    powershell -File "C:\Storage\Build\FFT Mod\port\native\play.ps1" -Join 192.168.1.20:7777

(or `-Join 127.0.0.1:7777` for a second window on the same PC). `-HostPort 7777` instead of `-Invite` waits for the guest and starts together. The title bar text says whose turn it is.

With the graphics-card viewer (`-Gl`): `-Gl -HostPort 7777` / `-Gl -Join HOST:7777` (both start together; joining a game in progress needs the CPU viewer for now),
and `-Gl -Two` for two players on one computer, controller 2 being the second gamepad. Network games start with an empty memory card on both sides (both copies of the game
must start from identical memory), so saves are not kept there.

**Gamepads** work in the `-Gl` viewer (controller 1 = keyboard or the first gamepad). The layout is positional like the PlayStation pad: bottom = Cross, right = Circle (confirm), left = Square,
top = Triangle, bumpers = L1/R1, triggers = L2/R2, Back = Select, Start = Start, d-pad or left stick = directions.

**Fast effects**: `-FastEffects` plays ability effects at 60 fps. The original game paces the battle at 15-30 fps while an effect plays (`battle_state_sync_frame` waits 2-4 vertical blanks per frame), which is faithful but slow; the option only changes how many blanks it waits in those two battle states, music keeps its timing. Not the original behaviour, so it is off by default.

**Test save states**: `python port\tools\make_test_states.py` makes F5 = the first battle starting, F6 = the end of that battle (the victory scene follows), F7 = the second battle's deployment screen. A state loads only into the build that wrote it: run the script again after rebuilding.

**Texture filter**: `-Gl -Filter` (or T while playing) smooths the game's textures at 2x-4x (EPX, as in the CPU viewer's HD mode).
Save / load states are disabled while two play. Both players need the same build and their own disc image.

## Where things are

`OVERNIGHT-REPORT.md` (what exists, newest first), `NATIVE-RUNTIME.md` (design and evidence), `port\README.md` (every script), `port\samples\native-frames\` (pictures),
`ROADMAP.md` / `PORT-FEASIBILITY.md` (plans). Nothing here has been pushed or published anywhere.
