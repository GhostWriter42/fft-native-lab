# Disassemble a range of the ORIGINAL machine code (MIPS R3000) from an extracted disc file, inside the toolchain container.
#   .\port\tools\disasm.ps1 -File WORLD/WORLD.BIN -Load 0x800e0000 -Start 0x800fff50 -Size 308
# -File is relative to fft_decomp\build\extracted\files; -Load is the address the file is loaded at (see target\*.yaml).
param([Parameter(Mandatory)][string]$File, [Parameter(Mandatory)][string]$Load, [Parameter(Mandatory)][string]$Start, [Parameter(Mandatory)][int]$Size)
$root = Split-Path $PSScriptRoot -Parent
$repo = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$loadN = [Convert]::ToUInt32($Load.Replace('0x', ''), 16)
$startN = [Convert]::ToUInt32($Start.Replace('0x', ''), 16)
$stop = $startN + $Size
docker run --rm --pull=never --volume "$($repo)\build\extracted\files:/disc:ro" fft-decomp-dev:local `
    mipsel-linux-gnu-objdump -D -b binary -m mips:3000 --adjust-vma=$loadN --start-address=$startN --stop-address=$stop "/disc/$File"
