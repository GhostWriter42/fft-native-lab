# Grow and (optionally) run a native slice.  Usage:
#   .\port\native\closure.ps1 -Sources src/battle/a.c,src/battle/b.c [-Allow '^(battle|main)_'] [-Max 400] [-Harness harness_x.c]
param([Parameter(Mandatory)][string[]]$Sources, [string]$Allow = '.', [int]$Max = 400, [string]$Harness = '', [string]$Skip = '')
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
docker run --rm --pull=never -e "ALLOW=$Allow" -e "MAX=$Max" -e "HARNESS=$Harness" -e "SKIP=$Skip" `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" fft-decomp-dev:local `
    sh /port/native/closure_run.sh @Sources
