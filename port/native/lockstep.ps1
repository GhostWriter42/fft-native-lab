# Whole-program lockstep: the ORIGINAL game (R3000 interpreter) and the NATIVE build of the same game boot side by side from main(),
# both on the same HLE of the SDK hardware layer, and their RAM is compared at every VSync (and at the first code-overlay load, where the
# modules that were not built natively begin).
#   .\port\native\lockstep.ps1 [-Frames 60] [-Log 0] [-Rebuild] [-NoDivFix] [-Cflags '-DSHOW_DIFFS=100']
# Needs: port\build\portable (run .\port\probe.ps1 first), extracted disc files (fft_decomp\build\extracted), the raw disc image, Docker.
param([int]$Frames = 60, [int]$Log = 0, [switch]$Rebuild, [switch]$NoDivFix, [string]$Cflags = '')
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
$bin  = Join-Path (Split-Path $root -Parent) 'game\Final Fantasy Tactics.bin'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols_pc.ld') --functions main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_stubs.py') $repo (Join-Path $nb 'stub_table.c') --native-prefix=native_ main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_fuzz.py') $repo $nb --native-prefix=native_ main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_hle.py') $repo (Join-Path $nb 'hle_generated.c') | Out-Host
$dirs = 'src/main src/battle src/psyq/libgpu src/psyq/libc src/psyq/libapi src/psyq/libetc src/psyq/libcd src/psyq/libspu src/psyq/libcard src/psyq/libpress src/psyq/suzuki'
$gameFlags = '-ftrivial-auto-var-init=zero -fno-omit-frame-pointer'
$divfixEnv = ''
if (-not $NoDivFix) { $divfixEnv = '1' }
$keyFile = Join-Path $nb 'ls_pc\build_key.txt'
$repHash = (Get-FileHash (Join-Path $PSScriptRoot 'replacements\replaced.txt') -Algorithm MD5).Hash
$key = "flags=$gameFlags divfix=$divfixEnv dirs=$dirs replaced=$repHash"
$have = ''
if (Test-Path $keyFile) { $have = (Get-Content $keyFile -Raw).Trim() }
if ($Rebuild -or $have -ne $key) {
    docker run --rm --pull=never -e "RENAME=1" -e "DIRS=$dirs" -e "DIVFIX=$divfixEnv" -e "OUT=/port/build/native/ls_pc" -e "LDFILE=/port/build/native/symbols_pc.ld" -e "EXTRA_CFLAGS=$gameFlags" `
        --volume "${root}:/port" fft-decomp-dev:local sh /port/native/boundary.sh | Out-Host
    Set-Content -Path $keyFile -Value $key
}
docker run --rm --pull=never -e "DIVFIX=$divfixEnv" -e "EXTRA_CFLAGS=-DMAX_FRAMES=$Frames -DLOG_LIMIT=$Log $gameFlags $Cflags" `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" --volume "${bin}:/disc.bin:ro" fft-decomp-dev:local `
    sh /port/native/build_run_lockstep.sh
exit $LASTEXITCODE
