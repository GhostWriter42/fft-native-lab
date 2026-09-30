# Generic function-level differential fuzzer: every decompiled game function in src/main + src/battle whose signature can be
# called generically, native build vs the ORIGINAL machine code (R3000 interpreter), on random arguments.
#   .\port\native\fuzz.ps1 [-Trials 40] [-From 0] [-To 100000] [-AutoInit] [-DivFix] [-Rebuild] [-Cflags '-DVERBOSE']
# PS1-address scheme: natively compiled functions are linked as native_<name>, the original names are bound to their PS1
# addresses and reached through x86 trampolines, so function pointers stored in RAM stay canonical PS1 addresses.
# -AutoInit  compile the game with -ftrivial-auto-var-init=zero (the recommended native flag: uninitialised locals read 0)
# -DivFix    give integer division MIPS semantics for zero and -1 divisors (tools/divfix.awk; see PORT-FEASIBILITY.md)
# -Rebuild   recompile all game sources natively first (~1-2 min); done automatically when the flags change.
# Needs: port\build\portable (run .\port\probe.ps1 first), extracted disc files (fft_decomp\build\extracted), Docker.
param([int]$Trials = 40, [int]$From = 0, [int]$To = 100000, [switch]$AutoInit, [switch]$DivFix, [switch]$Rebuild, [string]$Cflags = '')
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols_pc.ld') --functions main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_stubs.py') $repo (Join-Path $nb 'stub_table.c') --native-prefix=native_ main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_fuzz.py') $repo $nb --native-prefix=native_ main.yaml battle.yaml | Out-Host
$gameFlags = ''
if ($AutoInit) { $gameFlags = '-ftrivial-auto-var-init=zero' }
$keyFile = Join-Path $nb 'all_pc\build_key.txt'
$divfix = ""
if ($DivFix) { $divfix = "1" }
$key = "flags=$gameFlags divfix=$divfix"
$have = ''
if (Test-Path $keyFile) { $have = (Get-Content $keyFile -Raw).Trim() }
if ($Rebuild -or $have -ne $key) {
    docker run --rm --pull=never -e "RENAME=1" -e "DIVFIX=$divfix" -e "OUT=/port/build/native/all_pc" -e "LDFILE=/port/build/native/symbols_pc.ld" -e "EXTRA_CFLAGS=$gameFlags" `
        --volume "${root}:/port" fft-decomp-dev:local sh /port/native/boundary.sh | Out-Host
    Set-Content -Path $keyFile -Value $key
}
docker run --rm --pull=never -e "DIVFIX=$divfix" -e "EXTRA_CFLAGS=-DTRIALS=$Trials -DFROM=$From -DTO=$To $gameFlags $Cflags" `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" fft-decomp-dev:local `
    sh /port/native/build_run_fuzz.sh
