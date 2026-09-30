# Undefined-behaviour worklist for the native port (see PORT-FEASIBILITY.md, "Determinism hazards").
#   .\port\hazards.ps1              snapshot HEAD of fft_decomp, portify, compile with gcc -m32 -O2 + UB warnings, aggregate
#   .\port\hazards.ps1 -Rev master
param([string]$Rev = 'HEAD')
$root = $PSScriptRoot
& (Join-Path $root 'probe.ps1') -Rev $Rev -Mode m32W | Out-Null
python (Join-Path $root 'tools\hazards.py') (Join-Path $root 'build\probe-out\portable-m32W') (Join-Path $root 'HAZARDS.md')
