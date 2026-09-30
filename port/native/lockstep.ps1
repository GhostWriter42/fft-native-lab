# Whole-program lockstep: the ORIGINAL game (R3000 interpreter) and the NATIVE build of the same game boot side by side from main(),
# both on the same HLE of the SDK hardware layer, and their RAM is compared at every VSync (and at the first code-overlay load, where the
# modules that were not built natively begin).
#   .\port\native\lockstep.ps1 [-Frames 60] [-Log 0] [-Rebuild] [-NoDivFix] [-Scenario title] [-Cflags '-DSHOW_DIFFS=100']
# -Pad 'frame:buttons,...'   scripted controller 1 (buttons hold until the next entry), e.g. -Pad '600:0x800,606:0' presses START at frame 600 for 6 frames
# -TitleToBattle             the fixed START/CIRCLE presses that take a new game from the title menu into the first battle (frame ~1170); see padgen.ps1
# -PadSeed N [-PadFrom 1180] random play from that frame on (a different game for every seed; combine with -TitleToBattle)
# -PokeWhen 'cond_symbol=value:symbol=value,...'   cheats: once, when the word at the first symbol equals the value, store the second value at the second symbol (both machines), e.g.
#             'g_battle_game_state=0x27:g_battle_game_state=0x3b' closes the battle at the first change of turn (-> the world map)
# -Gpu  attach the software GPU (hle\gpu.c) to both machines: VRAM is compared at every frame; -Shot 'f1,f2' / -ShotEvery N [-ShotFrom F] write the native display as PNG to port\build\shots (-ShotScale 1..3)
# -NatWatch 'sym+off[:FROM_FRAME]'   report every store of the NATIVE game to that word (page protection + single step: slow), with the storing instruction resolved to a function
# -Replay N   at frame N run the ORIGINAL first, record its function-call sequence, then run the native game under live comparison: the first different call (or a hang) is reported
# -Dump N     with -Replay: also print the first N calls of the replayed frame (function, first two arguments)
# -Where N    at the VSync of frame N print where the original is (function, registers, the return addresses found on its stack)
# -Scenario title   steer the game past the opening movie (its CD-streaming/MDEC hardware is not modelled) to the title menu
# Needs: port\build\portable (run .\port\tools\mktree.ps1 first), extracted disc files (fft_decomp\build\extracted), the raw disc image, Docker.
# -Peek 'symbol:words,...'   dump memory (interpreter side) at these symbols when the original stops abnormally (with -Replay)
# -Watch 'g_symbol,g_other+4'   print the value (interpreter side) of these game variables (32-bit words at symbol[+offset]) whenever it changes, per frame; with -Replay the first four
#                               are also compared at every function entry (the first entry where the native game's value differs is reported)
# -BuildOnly  compile and link the program (kept in the docker volume) without running it;  -RunOnly  run the program linked by an earlier build (no generators, no
#             compilation: soak.ps1 uses this once per random input script). Build flags (-Scenario, -Replay, -Cflags, -Watch ...) are baked into the program.
param([int]$Frames = 60, [int]$Log = 0, [switch]$Rebuild, [switch]$NoDivFix, [string]$Scenario = '', [string]$Pad = '', [string]$Watch = '', [string]$Peek = '', [int]$Replay = 0, [int]$Dump = 0, [string]$DumpAround = '', [string]$Cflags = '', [int]$PadSeed = 0, [int]$PadFrom = 1180, [switch]$TitleToBattle, [switch]$RunOnly, [switch]$BuildOnly, [int]$Where = 0, [string]$PokeWhen = '', [string]$NatWatch = '', [switch]$Gpu, [string]$Shot = '', [int]$ShotEvery = 0, [int]$ShotFrom = 1, [int]$ShotScale = 1)
. (Join-Path $PSScriptRoot 'padgen.ps1')
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native\ls'      # own generated tables: the function fuzz keeps using build\native
$bin  = Join-Path (Split-Path $root -Parent) 'game\Final Fantasy Tactics.bin'
New-Item -ItemType Directory -Force $nb | Out-Null
# the compiled objects live in a docker volume (native filesystem: thousands of small files are ~10x faster than on the Windows bind mount)
$vol = if ($env:FFT_LS_VOL) { $env:FFT_LS_VOL } else { 'fft-ls-objs' }          # FFT_LS_VOL: another docker volume, for a build that must not disturb a running soak
# the run configuration (frames, controller script) is read by the program at start-up
$cfg = Join-Path $nb 'run.cfg'
[System.IO.File]::WriteAllText($cfg, (New-RunConfig -Frames $Frames -Pad $Pad -PadSeed $PadSeed -PadFrom $PadFrom -TitleToBattle:$TitleToBattle -PokeWhen $PokeWhen -NatWatch $NatWatch -Gpu:$Gpu -Shot $Shot -ShotEvery $ShotEvery -ShotFrom $ShotFrom -ShotScale $ShotScale))
$shots = Join-Path $root 'build\shots'
New-Item -ItemType Directory -Force $shots | Out-Null
$runVolumes = @('--volume', "${shots}:/shots", '--volume', "${root}:/port", '--volume', "${vol}:/ob", '--volume', "$($repo)\build\extracted\files:/disc:ro", '--volume', "${bin}:/disc.bin:ro", '--volume', "${cfg}:/run.cfg:ro")
if ($RunOnly) {
    docker run --rm --pull=never --cap-add SYS_RAWIO -e "RUN_ONLY=1" @runVolumes fft-decomp-dev:local sh /port/native/build_run_lockstep.sh
    exit $LASTEXITCODE
}
$mods = @('main.yaml', 'battle.yaml', 'opening.yaml', 'wldcore.yaml', 'world.yaml', 'event.yaml', 'effect.yaml')      # the modules built natively (main first); each needs its source dir in $dirs (event.yaml: 11 overlay documents, effect.yaml: 110)
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols_pc.ld') --functions --native-prefix=native_ @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_modules.py') $repo (Join-Path $nb 'modules.c') --native-prefix=native_ @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_fuzz.py') $repo $nb --native-prefix=native_ @mods | Out-Host
python (Join-Path $PSScriptRoot 'gen_hle.py') $repo (Join-Path $nb 'hle_generated.c') @mods | Out-Host
$dirs = 'src/main src/battle src/open src/wldcore src/world src/event src/effect src/psyq/libgpu src/psyq/libc src/psyq/libapi src/psyq/libetc src/psyq/libcd src/psyq/libspu src/psyq/libcard src/psyq/libpress src/psyq/suzuki'
# -finstrument-functions: the replay mode compares the native game's function entries with the original's (the runtime's own files are excluded)
$gameFlags = '-ftrivial-auto-var-init=zero -fno-omit-frame-pointer -finstrument-functions -finstrument-functions-exclude-file-list=native/rt.c,native/hle/hle.c,native/hle/gpu.c,native/r3000/r3000.c,native/gte/gte.c,native/lockstep.c,native/bios_rt.c,sym_table.c,modules.c'
$divfixEnv = ''
if (-not $NoDivFix) { $divfixEnv = '1' }
$repHash = (Get-FileHash (Join-Path $PSScriptRoot 'replacements\replaced.txt') -Algorithm MD5).Hash
$stampFile = Join-Path $root 'build\portable\.stamp'
$stamp = ''
if (Test-Path $stampFile) { $stamp = (Get-Content $stampFile -Raw).Trim() }
$key = "flags=$gameFlags divfix=$divfixEnv dirs=$dirs replaced=$repHash tree=$stamp"
$have = ((docker run --rm --pull=never --volume "${vol}:/ob" fft-decomp-dev:local sh -c 'cat /ob/ls_pc/build_key.txt 2>/dev/null') -join '').Trim()
if ($Rebuild -or $have -ne $key) {
    docker run --rm --pull=never -e "RENAME=1" -e "DIRS=$dirs" -e "DIVFIX=$divfixEnv" -e "OUT=/ob/ls_pc" -e "LDFILE=/port/build/native/ls/symbols_pc.ld" -e "FN=/port/build/native/ls/fn_names.txt" -e "SCOPED=/port/build/native/ls" -e "EXTRA_CFLAGS=$gameFlags" `
        --volume "${root}:/port" --volume "${vol}:/ob" fft-decomp-dev:local sh /port/native/boundary.sh | Out-Host
    docker run --rm --pull=never --volume "${vol}:/ob" fft-decomp-dev:local sh -c "printf '%s' '$key' > /ob/ls_pc/build_key.txt"
}
$scnFlag = ''
if ($Scenario -eq 'title') { $scnFlag = '-DSCENARIO_TITLE' }
if ($Replay -gt 0) { $scnFlag = "$scnFlag -DREPLAY_FRAME=$Replay" }
if ($Dump -gt 0) { $scnFlag = "$scnFlag -DEVENT_DUMP=$Dump" }
if ($Where -gt 0) { $scnFlag = "$scnFlag -DWHERE_FRAME=$Where" }
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
    if ($wn -match '^0x[0-9a-fA-F]+$') { $watchLines += ('{{ 0x{0:x8}u, "{1}" }},' -f ([uint32]([Convert]::ToUInt32($wn.Substring(2), 16) + $off)), $w); continue }       # a plain address
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
$buildEnv = @()
if ($BuildOnly) { $buildEnv = @('-e', 'BUILD_ONLY=1') }
docker run --rm --pull=never --cap-add SYS_RAWIO -e "DIVFIX=$divfixEnv" -e "EXTRA_CFLAGS=-DMAX_FRAMES=$Frames -DLOG_LIMIT=$Log -DLOCKSTEP_THREAD_WINDOW $scnFlag $gameFlags $Cflags" @buildEnv `
    @runVolumes fft-decomp-dev:local sh /port/native/build_run_lockstep.sh
exit $LASTEXITCODE
