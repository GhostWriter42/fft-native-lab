# Controller-input scripts for the whole-program lockstep (dot-sourced by lockstep.ps1 and soak.ps1).
#   New-RunConfig -Frames 8000 -Pad '600:0x800,606:0' -PadSeed 7 -PadFrom 1180  ->  the text of a /run.cfg ("frames N" + "pad FRAME BUTTONS" lines)
# -Pad is a fixed prefix ('frame:buttons,...', buttons hold until the next entry); -PadSeed appends random play from frame -PadFrom on: a button (or a d-pad + action
# combination) is held for a few frames, then released, over and over, with a weighting that resembles menu navigation in a tactics game.
# PlayStation pad bits (PadRead): SELECT 0x100, START 0x800, UP 0x1000, RIGHT 0x2000, DOWN 0x4000, LEFT 0x8000, L2 1, R2 2, L1 4, R1 8, TRIANGLE 0x10, CIRCLE 0x20, CROSS 0x40, SQUARE 0x80.

# the fixed prefix that gets a new game from the title menu into the first battle (START / CIRCLE presses, alternating every 40 frames from frame 600)
function New-TitleToBattlePad {
    $entries = @()
    for ($f = 600; $f -lt 1180; $f += 40) {
        $b = if ((($f - 600) / 40) % 2 -eq 0) { 0x800 } else { 0x20 }
        $entries += ,@($f, $b)
        $entries += ,@(($f + 6), 0)
    }
    return $entries
}

function New-RandomPad([int]$Seed, [int]$From, [int]$To) {
    $rng = [System.Random]::new($Seed)
    $dpad = @(0x1000, 0x2000, 0x4000, 0x8000)
    $entries = @()
    $f = $From
    while ($f -lt $To) {
        $r = $rng.NextDouble()
        $b = 0
        if ($r -lt 0.36) { $b = $dpad[$rng.Next(0, 4)] }
        elseif ($r -lt 0.60) { $b = 0x20 }                                       # CIRCLE: confirm
        elseif ($r -lt 0.70) { $b = 0x40 }                                       # CROSS: cancel
        elseif ($r -lt 0.76) { $b = 0x10 }                                       # TRIANGLE: menu
        elseif ($r -lt 0.80) { $b = 0x80 }                                       # SQUARE
        elseif ($r -lt 0.86) { $b = @(0x04, 0x08)[$rng.Next(0, 2)] }             # L1 / R1: camera
        elseif ($r -lt 0.89) { $b = 0x800 }                                      # START
        elseif ($r -lt 0.91) { $b = 0x100 }                                      # SELECT
        elseif ($r -lt 0.94) { $b = @(0x01, 0x02)[$rng.Next(0, 2)] }             # L2 / R2
        elseif ($r -lt 0.97) { $b = $dpad[$rng.Next(0, 4)] -bor 0x20 }           # d-pad + CIRCLE
        else { $b = 0 }
        $entries += ,@($f, $b)
        $f += $rng.Next(2, 12)
        $entries += ,@($f, 0)
        $f += $rng.Next(1, 8)
    }
    return $entries
}

# symbol name (or symbol+offset, or a number) -> address, from the linker script the last lockstep build generated
$script:ldSyms = $null
function Get-SymAddr([string]$name) {
    if ($name -match '^(0x[0-9a-fA-F]+|\d+)$') { return [uint32]$name }
    if ($null -eq $script:ldSyms) {
        $script:ldSyms = @{}
        $ld = Join-Path $PSScriptRoot '..\build\native\ls\symbols_pc.ld'
        foreach ($l in [System.IO.File]::ReadLines($ld)) { if ($l -match '^(\S+) = (0x[0-9a-fA-F]+);') { $script:ldSyms[$Matches[1]] = [uint32]$Matches[2] } }
    }
    $off = 0
    if ($name -match '^(\w+)\+(0x[0-9a-fA-F]+|\d+)$') { $name = $Matches[1]; $off = [uint32]$Matches[2] }
    if (-not $script:ldSyms.ContainsKey($name)) { throw "unknown symbol $name" }
    return [uint32]($script:ldSyms[$name] + $off)
}
# -PokeWhen 'cond_symbol=value:symbol=value[:repeat], ...': once (with :repeat, every time the condition holds), when the word at the first symbol equals the value, store the second value at the second symbol (both machines)
function New-PokeLines([string]$PokeWhen) {
    $lines = @()
    foreach ($e in ($PokeWhen -split ',' | Where-Object { $_ })) {
        $halves = $e -split ':'
        $c = $halves[0] -split '='; $t = $halves[1] -split '='
        $rep = if ($halves.Count -gt 2 -and $halves[2] -eq 'repeat') { 1 } else { 0 }
        $lines += ('pokewhen {0} {1} {2} {3} {4}' -f (Get-SymAddr $c[0]), [uint32]$c[1], (Get-SymAddr $t[0]), [uint32]$t[1], $rep)
    }
    return $lines
}

function New-RunConfig([int]$Frames, [string]$Pad = '', [int]$PadSeed = 0, [int]$PadFrom = 1180, [switch]$TitleToBattle, [string]$PokeWhen = '', [string]$NatWatch = '', [switch]$Gpu, [string]$Shot = '', [int]$ShotEvery = 0, [int]$ShotFrom = 1, [int]$ShotScale = 1, [string]$GpuWatch = '', [int]$PolyDump = 0, [string]$TexDump = '', [string]$SkipCmd = '') {
    $all = [System.Collections.Generic.List[object]]::new()
    $n = 0
    if ($Pad) {
        foreach ($e in ($Pad -split ',' | Where-Object { $_ })) { $q = $e -split ':'; $all.Add([pscustomobject]@{ F = [int]$q[0]; N = $n++; B = $q[1] }) }
    }
    if ($TitleToBattle) { foreach ($e in (New-TitleToBattlePad)) { $all.Add([pscustomobject]@{ F = [int]$e[0]; N = $n++; B = $e[1] }) } }
    if ($PadSeed -gt 0) { foreach ($e in (New-RandomPad $PadSeed $PadFrom $Frames)) { $all.Add([pscustomobject]@{ F = [int]$e[0]; N = $n++; B = $e[1] }) } }
    $lines = @("frames $Frames") + (New-PokeLines $PokeWhen)
    foreach ($c in ($SkipCmd -split ',' | Where-Object { $_ })) { $lines += "skipcmd $([int]$c)" }       # rasteriser debugging: polygon command bytes that are not drawn
    if ($TexDump) { $Gpu = [switch]$true; $lines += 'texdump ' + (($TexDump -split ',' | ForEach-Object { [int]$_ }) -join ' ') }     # frame,tpx,tpy,mode,clutx,cluty: a texture page decoded as /shots/tex<frame>.png
    if ($PolyDump) { $Gpu = [switch]$true; $lines += "polydump $PolyDump" }                               # print the tall textured polygons of that frame (rasteriser debugging)
    if ($GpuWatch) { $gw = $GpuWatch -split ','; $Gpu = [switch]$true; $lines += ('gpuwatch {0} {1}' -f [int]$gw[0], [int]$gw[1]) }      # log the writes to one VRAM pixel of both machines
    if ($Gpu -or $Shot -or $ShotEvery) { $lines += 'gpu 1' }                                    # software GPU: VRAM compared at every frame; screenshots (PNG) of the listed frames to port\build\shots
    foreach ($f in ($Shot -split ',' | Where-Object { $_ })) { $lines += "shot $([int]$f)" }
    if ($ShotEvery) { $lines += "shotevery $ShotEvery $ShotFrom" }
    if ($ShotScale -gt 1) { $lines += "shotscale $ShotScale" }
    if ($NatWatch) { $nw = $NatWatch -split ':'; $from = if ($nw.Count -gt 1) { [int]$nw[1] } else { 1 }; $lines += ('natwatch {0} {1}' -f (Get-SymAddr $nw[0]), $from) }      # every store of the native game to that word is reported (slow)
    # the program scans the entries in order: ascending frames (ties keep the order they were given in)
    foreach ($e in ($all | Sort-Object F, N)) { $lines += "pad $($e.F) $($e.B)" }
    return ($lines -join "`n") + "`n"
}
