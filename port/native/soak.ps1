# Random-play soak of the whole-program lockstep: many games with different random controller input, native build vs original machine code, RAM compared at every VSync.
#   .\port\native\soak.ps1 [-From 1] [-Count 20] [-Frames 8000] [-Parallel 6] [-PadFrom 1180] [-Rebuild]
# Builds the program once (lockstep.ps1 -Scenario title -BuildOnly), then runs it once per seed with `-TitleToBattle -PadSeed <seed>` (the fixed START/CIRCLE presses into the
# first battle, then random play from -PadFrom on). Logs: port\build\soak\seed_<n>.log; the summary is printed and written to port\build\soak\summary.txt.
param([int]$From = 1, [int]$Count = 20, [int]$Frames = 8000, [int]$Parallel = 6, [int]$PadFrom = 1180, [switch]$Rebuild, [switch]$NoBuild, [string]$PokeWhen = '')
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$bin  = Join-Path (Split-Path $root -Parent) 'game\Final Fantasy Tactics.bin'
$nb   = Join-Path $root 'build\native\ls'
$soak = Join-Path $root 'build\soak'
New-Item -ItemType Directory -Force $soak | Out-Null
. (Join-Path $PSScriptRoot 'padgen.ps1')
if (-not $NoBuild) {
    $a = @{ Scenario = 'title'; BuildOnly = $true; Frames = 60 }
    if ($Rebuild) { $a['Rebuild'] = $true }
    $buildOut = & (Join-Path $PSScriptRoot 'lockstep.ps1') @a *>&1 | Out-String
    $buildOut.Split("`n") | Select-Object -Last 6 | Out-Host
    if ($buildOut -notmatch 'linked /ob/ls_pc/prog') { throw 'build failed' }
}
$vol = 'fft-ls-objs'
$seeds = $From..($From + $Count - 1)
$sw = [Diagnostics.Stopwatch]::StartNew()
$results = $seeds | ForEach-Object -ThrottleLimit $Parallel -Parallel {
    $seed = $_
    $root = $using:root; $repo = $using:repo; $bin = $using:bin; $soak = $using:soak; $vol = $using:vol; $Frames = $using:Frames; $PadFrom = $using:PadFrom; $PokeWhen = $using:PokeWhen
    . (Join-Path $root 'native\padgen.ps1')
    $cfg = Join-Path $soak "cfg_$seed.txt"
    [System.IO.File]::WriteAllText($cfg, (New-RunConfig -Frames $Frames -PadSeed $seed -PadFrom $PadFrom -TitleToBattle -PokeWhen $PokeWhen))
    $log = Join-Path $soak "seed_$seed.log"
    $covDir = Join-Path $soak "cov_$seed"
    New-Item -ItemType Directory -Force $covDir | Out-Null
    $t0 = Get-Date
    docker run --rm --pull=never --cap-add SYS_RAWIO -e "RUN_ONLY=1" --volume "${root}:/port" --volume "${vol}:/ob" --volume "$($repo)\build\extracted\files:/disc:ro" --volume "${bin}:/disc.bin:ro" --volume "${cfg}:/run.cfg:ro" --volume "${covDir}:/cov" fft-decomp-dev:local sh /port/native/build_run_lockstep.sh *> $log
    $rc = $LASTEXITCODE
    $text = Get-Content $log
    $ok = [bool]($text | Select-String -CaseSensitive -Pattern '^== \d+ frames: RAM identical' -Quiet)
    $why = ''
    if (-not $ok) {
        $why = ($text | Select-String -CaseSensitive -Pattern '^(FRAME|NATIVE (CRASH|HANG)|original stopped|native main\(\) returned|the original did not)' | Select-Object -First 1).Line
        if (-not $why) { $why = "rc $rc, no verdict" }
        $sub = ($text | Select-String -CaseSensitive -Pattern '^    (g_|scratchpad|[A-Za-z_]+\+0x)' | Select-Object -First 3 | ForEach-Object { $_.Line.Trim() }) -join ' | '
        if ($sub) { $why += "  [$sub]" }
    }
    $lastFrame = ($text | Select-String -Pattern '^frame (\d+):' | Select-Object -Last 1)
    [pscustomobject]@{ Seed = $seed; Ok = $ok; Seconds = [int]((Get-Date) - $t0).TotalSeconds; Last = $(if ($lastFrame) { $lastFrame.Matches[0].Groups[1].Value } else { '-' }); Why = $why }
}
$results = $results | Sort-Object Seed
$lines = @()
foreach ($r in $results) { $lines += ('seed {0,4}  {1}  {2,3}s  last sync frame {3,5}  {4}' -f $r.Seed, $(if ($r.Ok) { 'PASS' } else { 'FAIL' }), $r.Seconds, $r.Last, $r.Why) }
$pass = @($results | Where-Object { $_.Ok }).Count
$lines += "== $pass of $($results.Count) seeds identical over $Frames frames ($([int]$sw.Elapsed.TotalSeconds) s)"
$lines | Out-Host
[System.IO.File]::WriteAllText((Join-Path $soak 'summary.txt'), (($lines -join "`n") + "`n"))

# NULL-page accesses of the native game: places where the retail code reads console RAM through a NULL pointer (each needs a reviewed source patch for a build that cannot map page zero)
$nullSites = @{}
foreach ($seed in $seeds) {
    $log = Join-Path $soak "seed_$seed.log"
    foreach ($l in (Select-String -Path $log -CaseSensitive -Pattern '^NULL-PAGE ACCESS' | ForEach-Object { $_.Line })) {
        foreach ($m in [regex]::Matches($l, '@0x[0-9a-f]+\(([^)+]+)\+(\d+)\)')) { $k = $m.Groups[1].Value; if (-not $nullSites.ContainsKey($k)) { $nullSites[$k] = 0 }; $nullSites[$k]++ }
    }
}
$nl = @("NULL-page accesses by native game code (function: seeds): " + $(if ($nullSites.Count) { '' } else { 'none' }))
foreach ($k in ($nullSites.Keys | Sort-Object)) { $nl += ('  {0}: {1}' -f $k, $nullSites[$k]) }
$nl | Out-Host
[System.IO.File]::WriteAllText((Join-Path $soak 'nullpage.txt'), (($nl -join "`n") + "`n"))

# function coverage of the native game over all seeds (the union): which functions ran at least once, per module. Every seed dumps /cov/hits.txt
# ("module NAME total N native N hit N" followed by "function h" for each function that ran); the functions that never ran are listed in coverage-unhit.txt
$hit = @{}; $total = [ordered]@{}; $native = @{}
foreach ($seed in $seeds) {
    $f = Join-Path $soak "cov_$seed\hits.txt"
    if (-not (Test-Path $f)) { continue }
    $mod = ''
    foreach ($l in [System.IO.File]::ReadLines($f)) {
        if ($l.StartsWith('module ')) { $q = $l.Split(' '); $mod = $q[1]; $total[$mod] = [int]$q[3]; $native[$mod] = [int]$q[5]; if (-not $hit.ContainsKey($mod)) { $hit[$mod] = [System.Collections.Generic.HashSet[string]]::new() } }
        elseif ($l.EndsWith(' h')) { [void]$hit[$mod].Add($l.Substring(0, $l.Length - 2)) }
    }
}
$cov = @('function coverage of the native game (union over the seeds): module   hit / with native code / all')
$all = 0; $allHit = 0
$fam = [ordered]@{}
foreach ($m in $total.Keys) {
    $f = if ($m -match '^(event|effect)_') { $Matches[1] + '_* (' + (@($total.Keys | Where-Object { $_ -like ($Matches[1] + '_*') }).Count) + ' overlays)' } else { $m }
    if (-not $fam.Contains($f)) { $fam[$f] = @(0, 0, 0) }
    $fam[$f][0] += $hit[$m].Count; $fam[$f][1] += $native[$m]; $fam[$f][2] += $total[$m]
    $all += $native[$m]; $allHit += $hit[$m].Count
}
foreach ($f in $fam.Keys) { $cov += ('  {0,-22} {1,5} / {2,5} / {3,5}' -f $f, $fam[$f][0], $fam[$f][1], $fam[$f][2]) }
$cov += ('  {0,-16} {1,5} / {2,5}' -f 'TOTAL', $allHit, $all)
$cov | Out-Host
[System.IO.File]::WriteAllText((Join-Path $soak 'coverage.txt'), (($cov -join "`n") + "`n"))
[System.IO.File]::WriteAllLines((Join-Path $soak 'coverage-hit.txt'), [string[]]@($hit.Keys | ForEach-Object { $m = $_; $hit[$m] | ForEach-Object { "$m $_" } } | Sort-Object))
