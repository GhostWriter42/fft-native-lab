# Regenerate the PORTIFIED source tree (port\build\portable) from a committed revision of fft_decomp WITHOUT the compile survey of probe.ps1:
# git archive -> tools\portify.py (pins/barriers/views stripped + the reviewed native patches of port\native\native_patches.py).
#   .\port\tools\mktree.ps1 [-Rev HEAD]
# Writes port\build\portable\.stamp (revision + hashes of portify.py and native_patches.py); the native build scripts put it in their cache keys.
param([string]$Rev = 'HEAD')
$root  = Split-Path $PSScriptRoot -Parent
$repo  = Join-Path (Split-Path $root -Parent) 'fft_decomp'
$build = Join-Path $root 'build'
$snap  = Join-Path $build 'snap'
$port  = Join-Path $build 'portable'
foreach ($dir in @($snap, $port)) { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir } ; New-Item -ItemType Directory $dir | Out-Null }
$tar = Join-Path $build 'snap.tar'
git -C $repo archive $Rev -o $tar
tar -xf $tar -C $snap
Remove-Item $tar
$short = (git -C $repo rev-parse --short $Rev)
"snapshot of $Rev ($short): $((Get-ChildItem $snap\src -Recurse -Filter *.c | Measure-Object).Count) C files"
python (Join-Path $PSScriptRoot 'portify.py') $snap $port --report (Join-Path $build 'portify-report.txt')
if ($LASTEXITCODE -ne 0) { throw 'portify failed' }
$h1 = (Get-FileHash (Join-Path $PSScriptRoot 'portify.py') -Algorithm MD5).Hash
$h2 = (Get-FileHash (Join-Path $root 'native\native_patches.py') -Algorithm MD5).Hash
Set-Content -Path (Join-Path $port '.stamp') -Value "rev=$short portify=$h1 patches=$h2"
"stamp: $(Get-Content (Join-Path $port '.stamp'))"
