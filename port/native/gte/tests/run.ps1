# Hosted unit tests for the software GTE (64-bit gcc + libc in the toolchain image).
#   .\port\native\gte\tests\run.ps1
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent    # ...\port
docker run --rm --pull=never --volume "${root}:/port" fft-decomp-dev:local `
    sh -c "gcc -O1 -Wall -Wextra -Wno-unused-parameter -o /tmp/t /port/native/gte/tests/test_gte.c /port/native/gte/gte.c -lm && /tmp/t"
