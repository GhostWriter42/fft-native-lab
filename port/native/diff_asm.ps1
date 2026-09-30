# Differential test of the native C replacements for the hand-assembled BATTLE routines against the original machine code.
#   .\port\native\diff_asm.ps1
# Needs: port\build\portable (run .\port\probe.ps1 first), extracted disc files (fft_decomp\build\extracted), Docker.
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols.ld') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') main.yaml battle.yaml | Out-Host
docker run --rm --pull=never -e "EXTRA_SRC=/port/native/replacements/battle_asm.c /port/native/replacements/battle_asm2.c" `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" fft-decomp-dev:local `
    sh /port/native/build_run_diff.sh harness_diff_asm.c
