# Whole-program lockstep: the ORIGINAL game (R3000 interpreter) and the NATIVE build of the same game boot side by side from main(),
# both on the same HLE of the SDK hardware layer, and their RAM is compared at every VSync (and at the first code-overlay load, where the
# modules that were not built natively begin).
#   .\port\native\lockstep.ps1 [-Frames 60] [-Log 0] [-Rebuild] [-NoDivFix] [-Scenario title] [-Cflags '-DSHOW_DIFFS=100']
# -Pad 'frame:buttons,...'   scripted controller 1 (buttons hold until the next entry), e.g. -Pad '600:0x800,606:0' presses START at frame 600 for 6 frames
# -Replay N   at frame N run the ORIGINAL first, record its function-call sequence, then run the native game under live comparison: the first different call (or a hang) is reported
# -Dump N     with -Replay: also print the first N calls of the replayed frame (function, first two arguments)
# -Scenario title   steer the game past the opening movie (its CD-streaming/MDEC hardware is not modelled) to the title menu
# Needs: port\build\portable (run .\port\probe.ps1 first), extracted disc files (fft_decomp\build\extracted), the raw disc image, Docker.
# -Peek 'symbol:words,...'   dump memory (interpreter side) at these symbols when the original stops abnormally (with -Replay)
# -Watch 'g_symbol,g_other'   print the value (interpreter side) of these game variables whenever it changes, per frame
param([int]$Frames = 60, [int]$Log = 0, [switch]$Rebuild, [switch]$NoDivFix, [string]$Scenario = '', [string]$Pad = '', [string]$Watch = '', [string]$Peek = '', [int]$Replay = 0, [int]$Dump = 0, [string]$DumpAround = '', [string]$Cflags = '')
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native\ls'      # own generated tables: the function fuzz keeps using build\native
$bin  = Join-Path (Split-Path $root -Parent) 'game\Final Fantasy Tactics.bin'
New-Item -ItemType Directory -Force $nb | Out-Null
$mods = @('main.yaml', 'battle.yaml', 'opening.yaml', 'wldcore.yaml', 'world.yaml', 'event.yaml')      # the modules built natively (main first); each needs its source dir in $dirs (event.yaml: 11 overlay documents)
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols_pc.ld') --functions @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_modules.py') $repo (Join-Path $nb 'modules.c') --native-prefix=native_ @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_fuzz.py') $repo $nb --native-prefix=native_ @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_hle.py') $repo (Join-Path $nb 'hle_generated.c') @mods | Out-Host
$dirs = 'src/main src/battle src/open src/wldcore src/world src/event src/psyq/libgpu src/psyq/libc src/psyq/libapi src/psyq/libetc src/psyq/libcd src/psyq/libspu src/psyq/libcard src/psyq/libpress src/psyq/suzuki'
# -finstrument-functions: the replay mode compares the native game's function entries with the original's (the runtime's own files are excluded)
$gameFlags = '-ftrivial-auto-var-init=zero -fno-omit-frame-pointer -finstrument-functions -finstrument-functions-exclude-file-list=native/rt.c,native/hle/hle.c,native/r3000/r3000.c,native/gte/gte.c,native/lockstep.c,native/bios_rt.c,sym_table.c,modules.c'
$divfixEnv = ''
if (-not $NoDivFix) { $divfixEnv = '1' }
# the compiled objects live in a docker volume (native filesystem: thousands of small files are ~10x faster than on the Windows bind mount)
$vol = 'fft-ls-objs'
$repHash = (Get-FileHash (Join-Path $PSScriptRoot 'replacements\replaced.txt') -Algorithm MD5).Hash
$stampFile = Join-Path $root 'build\portable\.stamp'
$stamp = ''
if (Test-Path $stampFile) { $stamp = (Get-Content $stampFile -Raw).Trim() }
$key = "flags=$gameFlags divfix=$divfixEnv dirs=$dirs replaced=$repHash tree=$stamp"
$have = ((docker run --rm --pull=never --volume "${vol}:/ob" fft-decomp-dev:local sh -c 'cat /ob/ls_pc/build_key.txt 2>/dev/null') -join '').Trim()
if ($Rebuild -or $have -ne $key) {
    docker run --rm --pull=never -e "RENAME=1" -e "DIRS=$dirs" -e "DIVFIX=$divfixEnv" -e "OUT=/ob/ls_pc" -e "LDFILE=/port/build/native/ls/symbols_pc.ld" -e "FN=/port/build/native/ls/fn_names.txt" -e "EXTRA_CFLAGS=$gameFlags" `
        --volume "${root}:/port" --volume "${vol}:/ob" fft-decomp-dev:local sh /port/native/boundary.sh | Out-Host
    docker run --rm --pull=never --volume "${vol}:/ob" fft-decomp-dev:local sh -c "printf '%s' '$key' > /ob/ls_pc/build_key.txt"
}
$scnFlag = ''
if ($Scenario -eq 'title') { $scnFlag = '-DSCENARIO_TITLE' }
if ($Replay -gt 0) { $scnFlag = "$scnFlag -DREPLAY_FRAME=$Replay" }
if ($Dump -gt 0) { $scnFlag = "$scnFlag -DEVENT_DUMP=$Dump" }
$daText = ''
if ($DumpAround) { $daText = '#define DUMP_AROUND "' + $DumpAround + '"' + "`n" }
[System.IO.File]::WriteAllText((Join-Path $nb 'dump_around.h'), $daText)
$entries = @('{ 0u, 0u }')
if ($Pad) { $entries = @($Pad -split ',' | Where-Object { $_ } | ForEach-Object { $q = $_ -split ':'; '{ ' + $q[0] + 'u, ' + $q[1] + 'u }' }) }
[System.IO.File]::WriteAllText((Join-Path $nb 'pad_script.h'), (($entries -join ', ') + "`n"))
$ld = Get-Content (Join-Path $nb 'symbols_pc.ld')
$watchLines = @()
foreach ($w in ($Watch -split '[,\s]+' | Where-Object { $_ })) {        # symbol or symbol+offset (hex 0x.. or decimal): the 32-bit word at that address
    $wm = [regex]::Match($w, '^(\w+)(?:\+(0x[0-9a-fA-F]+|\d+))?$')
    if (-not $wm.Success) { Write-Warning "cannot parse the watch $w"; continue }
    $wn = $wm.Groups[1].Value; $off = 0
    if ($wm.Groups[2].Success) { $off = [Convert]::ToUInt32($(if ($wm.Groups[2].Value.StartsWith('0x')) { $wm.Groups[2].Value.Substring(2) } else { [Convert]::ToString([int]$wm.Groups[2].Value, 16) }), 16) }
    $m = $ld | Select-String -Pattern ('^' + [regex]::Escape($wn) + ' = (0x[0-9a-fA-F]+);') | Select-Object -First 1
    if ($m) { $base = [Convert]::ToUInt32($m.Matches[0].Groups[1].Value.Substring(2), 16); $watchLines += ('{{ 0x{0:x8}u, "{1}" }},' -f ([uint32]($base + $off)), $w) } else { Write-Warning "unknown symbol to watch: $wn" }
}
[System.IO.File]::WriteAllText((Join-Path $nb 'watch.h'), (($watchLines -join "`n") + "`n"))
$peekLines = @()
foreach ($w in ($Peek -split '[,\s]+' | Where-Object { $_ })) {
    $pn = ($w -split ':')[0]; $pc = 16; if (($w -split ':').Count -gt 1) { $pc = [int]($w -split ':')[1] }
    $m = $ld | Select-String -Pattern ('^' + [regex]::Escape($pn) + ' = (0x[0-9a-fA-F]+);') | Select-Object -First 1
    if ($m) { $peekLines += ('{ ' + $m.Matches[0].Groups[1].Value + 'u, ' + $pc + ', "' + $pn + '" },') } else { Write-Warning "unknown symbol to peek: $pn" }
}
[System.IO.File]::WriteAllText((Join-Path $nb 'peek.h'), (($peekLines -join "`n") + "`n"))
docker run --rm --pull=never -e "DIVFIX=$divfixEnv" -e "EXTRA_CFLAGS=-DMAX_FRAMES=$Frames -DLOG_LIMIT=$Log -DLOCKSTEP_THREAD_WINDOW $scnFlag $gameFlags $Cflags" `
    --volume "${root}:/port" --volume "${vol}:/ob" --volume "$($repo)\build\extracted\files:/disc:ro" --volume "${bin}:/disc.bin:ro" fft-decomp-dev:local `
    sh /port/native/build_run_lockstep.sh
exit $LASTEXITCODE
