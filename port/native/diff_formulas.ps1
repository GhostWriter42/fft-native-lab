# Differential fuzz of the battle formulas: native build of every handler vs the ORIGINAL machine code (R3000 interpreter).
#   .\port\native\diff_formulas.ps1
# Needs: port\build\portable (run .\port\probe.ps1 first), extracted disc files (fft_decomp\build\extracted), Docker.
param([int]$Trials = 300, [string]$Replay = '', [string]$ExtraFlags = '')   # -Replay '37,1332' replays one trial with call tracing
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols.ld') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_stubs.py') $repo (Join-Path $nb 'stub_table.c') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'decode_handlers.py') $repo g_battle_formula_handlers 128 (Join-Path $nb 'formula_table.c') native_formula_handlers | Select-Object -Last 1 | Out-Host
$cflags = "-DTRIALS=$Trials"
if ($Replay) { $r = $Replay -split ','; $cflags = "-DTRIALS=$Trials -DONLY_ID=$($r[0]) -DONLY_TRIAL=$($r[1]) -finstrument-functions" }
$src = @(Get-ChildItem (Join-Path $root 'build\portable\src\battle') -Filter 'battle_formula_*.c' | ForEach-Object { "src/battle/$($_.Name)" })
$src += '/port/build/native/formula_table.c'
& (Join-Path $PSScriptRoot 'closure.ps1') -Sources $src -Max 600 -Harness harness_diff_formulas.c `
    -Extra '/port/native/r3000/r3000.c /port/native/gte/gte.c /port/native/rt.c /port/build/native/stub_table.c /port/build/native/func_addrs.c' -Cflags "$cflags $ExtraFlags"
