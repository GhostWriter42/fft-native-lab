# Native-portability probe for fft_decomp (see PORT-FEASIBILITY.md).
#   .\port\probe.ps1                 snapshot the current HEAD of fft_decomp, portify it, compile with gcc -m32
#   .\port\probe.ps1 -Rev master     probe another revision
#   .\port\probe.ps1 -Baseline       also compile the UNtransformed snapshot, for comparison
# Needs: Docker Desktop running (image fft-decomp-dev:local), host Python 3. Touches nothing in the repo;
# everything is written under port\build\ (safe to delete).
param([string]$Rev = 'HEAD', [switch]$Baseline, [string]$Mode = 'm32S', [switch]$Shim)

$root  = $PSScriptRoot
$repo  = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$build = Join-Path $root 'build'
$snap  = Join-Path $build 'snap'
$port  = Join-Path $build 'portable'

foreach ($dir in @($snap, $port)) { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir } ; New-Item -ItemType Directory $dir | Out-Null }

# 1. snapshot of the committed tree (no working-tree edits, no game files)
$tar = Join-Path $build 'snap.tar'
git -C $repo archive $Rev -o $tar
tar -xf $tar -C $snap
Remove-Item $tar
"snapshot of $Rev ($(git -C $repo rev-parse --short $Rev)): $((Get-ChildItem $snap\src -Recurse -Filter *.c | Measure-Object).Count) C files"

# 2. portify
python (Join-Path $root 'tools\portify.py') $snap $port --report (Join-Path $build 'portify-report.txt')

# 3. compile with the toolchain image's gcc
function Probe([string]$rootInContainer, [string]$tag, [bool]$useShim = $false) {
    $extra = @()
    if ($useShim) { $extra = @('-e', 'PRE=-I/port/native/shim -I/port/native/gte', '-e', 'PROBE_EXCLUDE=^src/psyq/') }
    docker run --rm --pull=never -e "PROBE_ROOT=$rootInContainer" @extra --volume "${root}:/port" fft-decomp-dev:local sh /port/tools/probe.sh $Mode $tag
}
""
Probe '/port/build/portable' "portable-$Mode$(if ($Shim) { '-shim' })" $Shim.IsPresent
if ($Baseline) { ""; Probe '/port/build/snap' "baseline-$Mode" }
""
"details: port\build\portify-report.txt (real asm list) and port\build\probe-out\<tag>\*.err"
