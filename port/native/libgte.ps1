# Native libgte test: the software GTE + libgte_native.c (+ the plain-C libgte members compiled from the repo) checked
# against independent references, using the game's own sine/sqrt/atan tables through the RAM image.
#   .\port\native\libgte.ps1
# Needs: port\build\portable (run .\port\probe.ps1 first), the extracted disc files (fft_decomp\build\extracted), Docker.
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols.ld') main.yaml battle.yaml | Out-Host
docker run --rm --pull=never `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" fft-decomp-dev:local `
    sh /port/native/build_run_libgte.sh harness_libgte.c
