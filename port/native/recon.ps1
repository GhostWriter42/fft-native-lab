# Reconnaissance for a native slice: which external functions are left unresolved?  Usage:
#   .\port\native\recon.ps1 -Sources src/battle/foo.c,... [-Top 60]   (paths relative to port\build\portable, or /port/... absolute)
param([Parameter(Mandatory)][string[]]$Sources, [int]$Top = 60)
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
docker run --rm --pull=never -e "TOP=$Top" --volume "${root}:/port" fft-decomp-dev:local sh /port/native/recon.sh @Sources
