# Native link boundary of the game-logic code (src/main + src/battle): compiles everything natively (parallel) and lists the
# externals that are called but not defined by game code -- the SDK/BIOS surface a native port has to provide.
#   .\port\native\boundary.ps1
# Needs: port\build\portable (run .\port\probe.ps1 -Shim first so the GTE shim is applied), Docker.
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_symbols.py') $repo (Join-Path $nb 'symbols.ld') main.yaml battle.yaml | Out-Host
docker run --rm --pull=never --volume "${root}:/port" fft-decomp-dev:local sh /port/native/boundary.sh
