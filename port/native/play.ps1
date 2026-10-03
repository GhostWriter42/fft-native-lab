# Play the NATIVE build of the game in a window (prototype; needs Docker Desktop running and the host Python with Pillow + tkinter).
#   .\port\native\play.ps1 [-Hd 2..4] [-Scale 3] [-WorldCheat] [-Verify] [-Build] [-Gl]
# The native game (the decomp's C, compiled with a modern compiler) runs in the toolchain container on the HLE of the SDK hardware layer and the software GPU; the window shows its
# display -- with -Hd the polygons are rendered on a 2x..4x finer grid and the textures are EPX-filtered -- and sends the keyboard back as the controller.
# -Verify  also runs the ORIGINAL machine code next to it (R3000 interpreter) and compares RAM and VRAM at every frame: the game stops with a report if they ever differ (slower)
# Keys: arrows, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L1/R1, E/R = L2/R2, Enter = Start, Backspace = Select, P = pause, Tab = fast forward, F12 = screenshot,
#       F1-F4 save the whole machine state to a slot, F5-F8 load it.  No sound (SPU/XA are not modelled yet), movies are skipped.
# -Gl  draw with the graphics card (OpenGL; -Hd 1..4 is then the internal resolution factor, default 2): play_gl.py
# -Build   (re)compile and link the program first (automatic when there is none yet); -WorldCheat  skip the first battle (jump to the world map)
# Two players (every player runs their own copy of the game; only the controllers cross the network -- see netplay.py):
#   .\port\native\play.ps1 -HostPort 7777        wait for the other player, then start together (you are player 1)
#   .\port\native\play.ps1 -Invite 7777          start alone; the other player is taken into the game in progress when they connect (you are player 1)
#   .\port\native\play.ps1 -Join 192.168.1.20:7777   join the other player (you are player 2); -Delay N input delay in frames (default 6)
#   the host decides the rules: -Seat2 MASK (battle unit slots player 2 plays, default 0x1a) -Hotseat MASK (slots made player-controlled, default 0x1e = the AI allies of the first battle) -RandomBattle
# FFT_LS_VOL selects the docker volume that holds the compiled program (default fft-ls-objs).
param([int]$Hd = 0, [int]$Scale = 3, [switch]$Build, [switch]$WorldCheat, [switch]$Verify, [int]$HostPort = 0, [int]$Invite = 0, [string]$Join = '', [int]$Delay = 6, [string]$Seat2 = '0x1a', [string]$Hotseat = '0x1e', [switch]$RandomBattle, [switch]$Gl, [switch]$Compare, [switch]$Record)
. (Join-Path $PSScriptRoot 'padgen.ps1')
$vol = if ($env:FFT_LS_VOL) { $env:FFT_LS_VOL } else { 'fft-ls-objs' }
$have = ((docker run --rm --pull=never --volume "${vol}:/ob" fft-decomp-dev:local sh -c 'test -x /ob/ls_pc/prog && echo yes' 2>$null) -join '').Trim()
if ($Build -or $have -ne 'yes') {
    Write-Host "compiling the native game (a few minutes the first time) ..."
    & (Join-Path $PSScriptRoot 'lockstep.ps1') -BuildOnly -Scenario title -Gpu | Select-Object -Last 3 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Error 'the build failed'; exit 1 }
}
if ($HostPort -or $Invite -or $Join) {
    $pyArgs = @('--scale', $Scale, '--delay', $Delay)
    if ($Hd -ge 2) { $pyArgs += @('--hd', $Hd) }
    if ($HostPort) { $pyArgs += @('--host', $HostPort, '--seat2', $Seat2, '--hotseat', $Hotseat) }
    if ($Invite) { $pyArgs += @('--invite', $Invite, '--seat2', $Seat2, '--hotseat', $Hotseat) }
    if ($Join) { $pyArgs += @('--join', $Join) }
    if ($RandomBattle) { $pyArgs += '--random-battle' }
    python (Join-Path $PSScriptRoot 'play.py') @pyArgs
    exit $LASTEXITCODE
}
# the run configuration: the cheats and the HD factor, and "play N" last (it makes the driver interactive)
$poke = if ($WorldCheat) { 'g_battle_game_state=0x27:g_battle_game_state=0x3b' } else { '' }
$cfgDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\native\ls'
New-Item -ItemType Directory -Force $cfgDir | Out-Null
$cfg = Join-Path $cfgDir 'play.cfg'
$text = New-RunConfig -Frames 0 -PokeWhen $poke -Gpu -Hd $Hd
$mode = if ($Verify) { 1 } else { 2 }
if ($Gl) {                                                     # the graphics card draws: the container sends its GPU command trace, play_gl.py (OpenGL) replays it
    $venvPy = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\venv\Scripts\python.exe'
    if (-not (Test-Path $venvPy)) { Write-Error 'the OpenGL viewer needs the venv: python -m venv port\build\venv ; port\build\venv\Scripts\pip install moderngl glfw numpy pillow'; exit 1 }
    [System.IO.File]::WriteAllText($cfg, ($text.TrimEnd() + "`naudio 1`ngltrace 1`nplay 2`n"))
    $glScale = if ($Hd -ge 1) { $Hd } else { 2 }
    $glArgs = @('--scale', $glScale, '--cfg', $cfg)
    if ($Compare) { $glArgs += '--compare' }
    if ($Record) { $glArgs += '--record' }
    & $venvPy (Join-Path $PSScriptRoot 'play_gl.py') @glArgs
    exit $LASTEXITCODE
}
[System.IO.File]::WriteAllText($cfg, ($text.TrimEnd() + "`naudio 1`nplay $mode`n"))
python (Join-Path $PSScriptRoot 'play.py') --scale $Scale --cfg $cfg
