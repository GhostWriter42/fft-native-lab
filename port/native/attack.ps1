# Native spike #3: a complete plain weapon attack (battle formula 1) resolved headlessly by the game's own code.
#   .\port\native\attack.ps1
# Needs: port\build\portable (run .\port\probe.ps1 first), the extracted disc files (fft_decomp\build\extracted), Docker.
# 244 source files (the 209 battle_formula_* functions and everything they call) are compiled natively; only abs/rand
# (libc / BIOS) come from the harness.
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null

python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols.ld') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'decode_handlers.py') $repo g_battle_formula_handlers 128 (Join-Path $nb 'formula_table.c') native_formula_handlers | Select-Object -Last 1 | Out-Host

$src = @(Get-ChildItem (Join-Path $root 'build\portable\src\battle') -Filter 'battle_formula_*.c' | ForEach-Object { "src/battle/$($_.Name)" })
$src += '/port/build/native/formula_table.c'
& (Join-Path $PSScriptRoot 'closure.ps1') -Sources $src -Max 600 -Harness harness_attack.c
