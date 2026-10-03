# Playing the native build (prototype)

What this is: the decomp's C code compiled with a modern compiler, running next to nothing else but a small platform layer (software GPU, software sound chip, CD reader).
It is **not an emulator** and it needs your own disc image in `game\` (already there). It runs inside the Docker toolchain container; a small Python window shows
the picture, plays the sound and sends your keyboard back. Windows, Docker Desktop running, Python with Pillow and tkinter (already present on this machine).

## One player

    powershell -File "C:\Storage\Build\FFT Mod\port\native\play.ps1"

The first run compiles the game (a few minutes). Options: `-Hd 2` / `-Hd 4` (3D drawn at 2x / 4x resolution and the textures edge-smoothed; software rendering, so slow:
roughly 15 frames per second at 2x), `-WorldCheat` (skip the first battle, go to the world map), `-Verify` (also run the original machine code beside it and compare every frame --
the game stops with a report if they ever differ), `-Build` (recompile).

Keys: arrows = d-pad, **Z** = Cross (cancel), **X** = Circle (confirm), A = Square, S = Triangle (menu), Q/W = L1/R1, E/R = L2/R2, **Enter** = Start, Backspace = Select,
P = pause, Tab (held) = fast forward, F12 = screenshot (`port\build\shots`), **F1-F4 save the whole machine state, F5-F8 load it** (save anywhere, also in battle; files in `port\build\states`).
On the title screen press Enter, then X a few times; the intro movies are skipped.

Sound: the music and effects play through Windows audio (`--mute` with `play.py` to switch off). It has not been listened to by the author of this build: if it sounds wrong, the
numbers in `NATIVE-RUNTIME.md` (Result 7) say what is and is not modelled.

## Two players (co-op prototype)

Each player runs their own copy; only controller values cross the network (see `port\native\netplay.py`). Player 1 plays the player-controlled units and the menus,
player 2 plays the turns of some of the AI allies (`-Seat2 MASK`, default units 1, 3, 4 of the first battle; `-Hotseat MASK` says which units become player-controlled).

    powershell -File "C:\Storage\Build\FFT Mod\port\native\play.ps1" -Invite 7777

starts playing alone and takes the other player into the game in progress when they connect;

    powershell -File "C:\Storage\Build\FFT Mod\port\native\play.ps1" -Join 192.168.1.20:7777

(or `-Join 127.0.0.1:7777` for a second window on the same PC). `-HostPort 7777` instead of `-Invite` waits for the guest and starts together. The title bar text says whose turn it is.
Save / load states are disabled while two play. Both players need the same build and their own disc image.

## Where things are

`OVERNIGHT-REPORT.md` (what exists, newest first), `NATIVE-RUNTIME.md` (design and evidence), `port\README.md` (every script), `port\samples\native-frames\` (pictures),
`ROADMAP.md` / `PORT-FEASIBILITY.md` (plans). Nothing here has been pushed or published anywhere.
