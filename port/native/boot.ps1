# Boot probe: the game's ORIGINAL boot code on the R3000 interpreter with the SDK hardware layer replaced by HLE hooks.
#   .\port\native\boot.ps1 [-Frames 120] [-Log 400]
# Reads SCUS_942.21 from the extracted files and the CD by LBA from the raw image (game\Final Fantasy Tactics.bin).
param([int]$Frames = 120, [int]$Log = 400)
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
$bin  = Join-Path (Split-Path $root -Parent) 'game\Final Fantasy Tactics.bin'
New-Item -ItemType Directory -Force $nb | Out-Null
python (Join-Path $PSScriptRoot 'gen_funcs.py') $repo (Join-Path $nb 'func_addrs.c') main.yaml battle.yaml | Out-Host
python (Join-Path $PSScriptRoot 'gen_fuzz.py') $repo $nb --native-prefix=native_ main.yaml battle.yaml | Out-Host
docker run --rm --pull=never -e "EXTRA_CFLAGS=-DMAX_FRAMES=$Frames -DLOG_LIMIT=$Log" `
    --volume "${root}:/port" --volume "$($repo)\build\extracted\files:/disc:ro" --volume "${bin}:/disc.bin:ro" fft-decomp-dev:local `
    sh /port/native/build_run_boot.sh
