# Play the NATIVE build of the game in a window (prototype; needs Docker Desktop running and the host Python with Pillow + tkinter).
#   .\port\native\play.ps1 [-Hd 2..4] [-Scale 3] [-WorldCheat] [-Verify] [-Build]
# The native game (the decomp's C, compiled with a modern compiler) runs in the toolchain container on the HLE of the SDK hardware layer and the software GPU; the window shows its
# display -- with -Hd the polygons are rendered on a 2x..4x finer grid -- and sends the keyboard back as the controller.
# -Verify  also runs the ORIGINAL machine code next to it (R3000 interpreter) and compares RAM and VRAM at every frame: the game stops with a report if they ever differ (slower)
# Keys: arrows, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L1/R1, E/R = L2/R2, Enter = Start, Backspace = Select.  No sound (SPU/XA are not modelled yet), movies are skipped.
# -Build   (re)compile and link the program first (automatic when there is none yet); -WorldCheat  skip the first battle (jump to the world map)
# FFT_LS_VOL selects the docker volume that holds the compiled program (default fft-ls-objs).
param([int]$Hd = 0, [int]$Scale = 3, [switch]$Build, [switch]$WorldCheat, [switch]$Verify)
. (Join-Path $PSScriptRoot 'padgen.ps1')
$vol = if ($env:FFT_LS_VOL) { $env:FFT_LS_VOL } else { 'fft-ls-objs' }
$have = ((docker run --rm --pull=never --volume "${vol}:/ob" fft-decomp-dev:local sh -c 'test -x /ob/ls_pc/prog && echo yes' 2>$null) -join '').Trim()
if ($Build -or $have -ne 'yes') {
    Write-Host "compiling the native game (a few minutes the first time) ..."
    & (Join-Path $PSScriptRoot 'lockstep.ps1') -BuildOnly -Scenario title -Gpu | Select-Object -Last 3 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Error 'the build failed'; exit 1 }
}
# the run configuration: the cheats and the HD factor, and "play 1" last (it makes the driver interactive)
$poke = if ($WorldCheat) { 'g_battle_game_state=0x27:g_battle_game_state=0x3b' } else { '' }
$cfgDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\native\ls'
New-Item -ItemType Directory -Force $cfgDir | Out-Null
$cfg = Join-Path $cfgDir 'play.cfg'
$text = New-RunConfig -Frames 0 -PokeWhen $poke -Gpu -Hd $Hd
$mode = if ($Verify) { 1 } else { 2 }
[System.IO.File]::WriteAllText($cfg, ($text.TrimEnd() + "`nplay $mode`n"))
python (Join-Path $PSScriptRoot 'play.py') --scale $Scale --cfg $cfg
