# Native spike #2: PS1 "RAM image" at 0x80000000 -- real weapon data from your disc through the game's own symbols.
#   .\port\native\ram.ps1
# Needs: port\build\portable (run .\port\probe.ps1), extracted disc files (fft_decomp\build\extracted), Docker.
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols.ld') main.yaml battle.yaml
docker run --rm --pull=never `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" fft-decomp-dev:local `
    sh /port/native/build_run_ram.sh harness_ram.c `
    src/battle/battle_formula_calculate_base_xa.c src/battle/battle_formula_get_random_0_7fff.c
