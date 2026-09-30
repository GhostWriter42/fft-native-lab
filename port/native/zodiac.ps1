# Native spike #1: run the game's own zodiac-compatibility formula as 32-bit native code.
#   .\port\native\zodiac.ps1
# Needs port\build\portable (run .\port\probe.ps1 first), the extracted disc files under
# fft_decomp\build\extracted (created by `.\fft.ps1 build disc` / `extract`), and Docker Desktop.
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$nb   = Join-Path $root 'build\native'
New-Item -ItemType Directory -Force $nb | Out-Null

# 1. the real table from the disc: g_battle_zodiac_compatibility_modifiers @ 0x8018f600 in BATTLE.BIN (load 0x80067000)
python (Join-Path $PSScriptRoot 'extract_table.py') `
    (Join-Path $repo 'build\extracted\files\BATTLE.BIN') 0x80067000 0x8018f600 12 (Join-Path $nb 'zodiac_table.inc')
if ($LASTEXITCODE -ne 0) { throw 'table extraction failed' }

# 2. compile the game function + harness for 32-bit inside the toolchain image, link freestanding, run
docker run --rm --pull=never --volume "${root}:/port" fft-decomp-dev:local sh /port/native/build_run.sh `
    harness_zodiac.c src/battle/battle_formula_apply_zodiac_compatibility.c
