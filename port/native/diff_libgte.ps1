# Differential test of the native libgte against the ORIGINAL libgte machine code (run on the R3000 interpreter).
#   .\port\native\diff_libgte.ps1
# Needs: port\build\portable (run .\port\probe.ps1 first), extracted disc files (fft_decomp\build\extracted), Docker.
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols.ld') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') main.yaml battle.yaml | Out-Host
docker run --rm --pull=never `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" fft-decomp-dev:local `
    sh /port/native/build_run_diff.sh harness_diff_libgte.c
