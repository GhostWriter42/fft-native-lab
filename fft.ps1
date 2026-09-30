# Windows stand-in for `make` in fft_decomp/ (which has no Windows make).
# Mirrors the Makefile's DOCKER_RUN / TOOLS macros; runs everything in Docker.
#
# Usage (from anywhere):
#   .\fft.ps1 bootstrap               build the Docker image (first time, ~1.5 GB)
#   .\fft.ps1 validate [--module=x]   compile everything, compare with target/ hashes (no BIN needed)
#   .\fft.ps1 build disc              full byte-exact build -> fft_decomp\build\disc\output-scus-94221.*
#   .\fft.ps1 build <module>          build one module
#   .\fft.ps1 diff <FUNC>             compare one function with the original bytes
#   .\fft.ps1 test                    go vet + go test on the repo's tooling
#   .\fft.ps1 fmt-check | fmt         clang-format check / rewrite of src/ and include/
#   .\fft.ps1 check-config | extract | checksums | config-fmt | ...
#   .\fft.ps1 shell                   interactive shell inside the image
# Any other argument list is passed straight to the repo's Go tool (see `make help`).
#
# Needs: Docker Desktop running, scus-94221.bin in fft_decomp\ (copy of game\*.bin).
# Note: the repo must be checked out with LF endings (`git config core.autocrlf false`
# inside fft_decomp), or check-config rejects target/*.yaml.
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $ToolArgs)

# FFT_REPO points the helper at another checkout/worktree of fft_decomp (default: .\fft_decomp).
$repo  = if ($env:FFT_REPO) { $env:FFT_REPO } else { Join-Path $PSScriptRoot 'fft_decomp' }
$image = 'fft-decomp-dev:local'

if (-not $ToolArgs) { $ToolArgs = @('--help') }

if ($ToolArgs[0] -eq 'bootstrap') {
    docker build --tag $image $repo
    exit $LASTEXITCODE
}

docker image inspect $image *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Error "Docker image $image not found. Run: .\fft.ps1 bootstrap"
    exit 1
}

if ($ToolArgs[0] -eq 'shell') {
    docker run --rm --init --pull=never --interactive --tty --volume "${repo}:/work" $image bash
    exit $LASTEXITCODE
}

# `make test`: vet and test the Go tooling.
if ($ToolArgs[0] -eq 'test') {
    docker run --rm --init --pull=never --volume "${repo}:/work" $image sh -c 'cd tools && go vet ./... && go test ./...'
    exit $LASTEXITCODE
}

# `make fmt` / `make fmt-check`: clang-format from the image, so every host formats identically.
# Note: formatting can change the bytes (cc1 emits line notes), so run `validate` after `fmt`.
if ($ToolArgs[0] -in @('fmt', 'fmt-check')) {
    # Typed array: an `if` expression would unroll a one-element array to a bare string, and
    # splatting that mangles the flag (clang-format then saw "i" as a filename).
    [string[]]$flags = @('--dry-run', '--Werror')
    if ($ToolArgs[0] -eq 'fmt') { $flags = @('-i') }
    $fmt = 'find src include -type f \( -name "*.c" -o -name "*.h" \) -print0 | sort -z | xargs -0 -r clang-format --style=file "$@"'
    docker run --rm --init --pull=never --volume "${repo}:/work" $image bash -o pipefail -c $fmt clang-format @flags
    exit $LASTEXITCODE
}

# Same in-container command as the Makefile's TOOLS macro.
$inner = 'go build -C tools -o ../build/bin/tools . && cp build/bin/tools /tmp/tools && exec /tmp/tools "$@"'

# Pass any TOOLS_* variables from the host (e.g. TOOLS_MOD_MODE=1 for the opt-in modding build) into the container.
$envArgs = @()
foreach ($e in (Get-ChildItem Env: | Where-Object { $_.Name -like 'TOOLS_*' })) { $envArgs += '--env'; $envArgs += "$($e.Name)=$($e.Value)" }

docker run --rm --init --pull=never @envArgs --volume "${repo}:/work" $image sh -c $inner tools @ToolArgs
exit $LASTEXITCODE
