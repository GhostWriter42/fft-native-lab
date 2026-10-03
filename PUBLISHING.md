# Publishing plan (nothing here has been pushed)

The work splits in two, and the two never share a branch.

## 1. Upstream candidates -- a fork of `adamrt/fft_decomp`

Vanilla, byte-exact, reviewable in isolation. Each branch is based on upstream `master` and must pass `validate` and the byte-for-byte diff before it is pushed.

| Branch | Content |
|---|---|
| `windows-line-endings` | `.gitattributes` and a README note on the CRLF pitfall |
| `remove-unneeded-pins-and-barriers` | removes 172 workarounds from the reconstructed SDK; build disc identical |
| `document-native-port-ub` | `QUIRKS.md` only: undefined behaviour and retail-ABI accidents that a native build must neutralise |

These are the only branches meant for pull requests. No source change goes on them unless every function still byte-matches.

## 2. Experiments -- our own repositories (public)

* `mod-build-path` (opt-in modding build, example mod): downstream only, a branch in the fork that is never proposed upstream.
* The project-root repository (the native port: `port/`, the docs, the helper scripts): not a fork, since it has no upstream. It is exported with `port/tools/export_public.py`.

## What never goes public (`PUBLIC-EXCLUDE.txt`)

* Pictures made from the game (`port/samples/`: screenshots, decoded sprite sheets, upscaled sprites).
* `patches/` (format-patch exports containing pieces of the decomp's source; the branches live in the fork).
* `port/native/replacements/battle_asm3.c`: a mechanical translation of the retail machine code. `python port/native/regen_asm3.py` rebuilds it, byte for byte, from your own disc.
* Output files listing values read from the game's data tables.
* The disc image, extracted files, save states and every build output (already ignored by `.gitignore`).

`port/tools/export_public.py OUT_DIR --name ... --email ...` makes a fresh clone, removes those paths from every commit, re-authors the commits, drops the remote and verifies the result (no excluded path, picture, audio, disc or state file in any revision, no blob over 1 MB). It does not push.

A judgement call to keep in mind: the hand-written C versions of routines that the retail binary hand-assembles (`battle_asm.c`, `battle_asm2.c`, `world_asm.c`, `main_asm.c`) stay in the export. They are our own reimplementations, like the decomp itself, which is public. If you would rather not publish them, add them to `PUBLIC-EXCLUDE.txt`.

## Before anything is pushed

1. The commit identity (name and email) is chosen and the export is made with it. The history currently carries `Test User <test@example.com>`.
2. A licence is chosen for our own code.
3. The repositories are created by the owner of the GitHub account, who also runs the push.
