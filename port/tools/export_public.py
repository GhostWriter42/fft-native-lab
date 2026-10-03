#!/usr/bin/env python3
"""Make a publishable copy of this repository: a fresh clone with the paths in PUBLIC-EXCLUDE.txt removed from EVERY commit
(not just the latest) and, optionally, every commit re-authored. The working repository is not touched.

    python port/tools/export_public.py OUT_DIR [--name "Your Name" --email you@example.com] [--branch main]

It then verifies the copy: no excluded path in any revision, no picture / audio / disc-image / save-state file in any revision,
no blob over 1 MB, and prints the author list. It never pushes anything.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN_EXT = ('.png', '.jpg', '.jpeg', '.gif', '.bmp', '.wav', '.mp3', '.ogg', '.bin', '.iso', '.img', '.cue', '.state', '.tim', '.spr', '.shp')
MAX_BLOB = 1_000_000


def git(args, cwd, env=None, check=True):
    r = subprocess.run(['git'] + args, cwd=cwd, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if check and r.returncode != 0:
        sys.exit(f'git {" ".join(args)} failed:\n{r.stderr}')
    return r.stdout


def read_excludes():
    out = []
    for line in (ROOT / 'PUBLIC-EXCLUDE.txt').read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--name')
    ap.add_argument('--email')
    ap.add_argument('--branch', default='')
    args = ap.parse_args()
    out = Path(args.out).resolve()
    if out.exists() and any(out.iterdir()):
        sys.exit(f'{out} exists and is not empty; refusing to overwrite it')
    if bool(args.name) != bool(args.email):
        sys.exit('give --name and --email together')
    excludes = read_excludes()

    git(['clone', '--quiet', '--no-hardlinks', '-c', 'core.autocrlf=false', str(ROOT), str(out)], cwd=ROOT.parent)
    git(['remote', 'remove', 'origin'], cwd=out)                      # the copy must not point back at (or push to) anything
    env = dict(os.environ, FILTER_BRANCH_SQUELCH_WARNING='1')
    quoted = ' '.join('"%s"' % p for p in excludes)
    cmd = ['filter-branch', '-f', '--prune-empty', '--index-filter', f'git rm -r -q --cached --ignore-unmatch -- {quoted}']
    if args.name:
        n, e = args.name, args.email
        cmd += ['--env-filter', f'GIT_AUTHOR_NAME="{n}"; GIT_AUTHOR_EMAIL="{e}"; GIT_COMMITTER_NAME="{n}"; GIT_COMMITTER_EMAIL="{e}"']
    cmd += ['--', '--all']
    git(cmd, cwd=out, env=env)
    shutil.rmtree(out / '.git' / 'refs' / 'original', ignore_errors=True)
    git(['reflog', 'expire', '--expire=now', '--all'], cwd=out)
    git(['gc', '--quiet', '--prune=now', '--aggressive'], cwd=out)

    # ---- verification, over every revision
    problems = []
    objs = git(['rev-list', '--all', '--objects'], cwd=out).splitlines()
    paths = {l.split(' ', 1)[1] for l in objs if ' ' in l}
    for p in sorted(paths):
        low = p.lower()
        if any(low == x.rstrip('/').lower() or (x.endswith('/') and low.startswith(x.lower())) for x in excludes):
            problems.append(f'excluded path still present: {p}')
        if low.endswith(FORBIDDEN_EXT):
            problems.append(f'forbidden file type in history: {p}')
    batch = subprocess.run(['git', 'cat-file', '--batch-all-objects', '--batch-check'], cwd=out, capture_output=True, text=True).stdout
    for line in batch.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == 'blob' and int(parts[2]) > MAX_BLOB:
            problems.append(f'blob over {MAX_BLOB} bytes: {parts[0]}')
    authors = sorted(set(git(['log', '--all', '--format=%an <%ae> | %cn <%ce>'], cwd=out).splitlines()))
    ncommits = len(git(['rev-list', '--all'], cwd=out).split())
    print(f'exported {ncommits} commits, {len(paths)} distinct paths -> {out}')
    print('authors / committers in the copy:')
    for a in authors:
        print('  ', a)
    if problems:
        print('PROBLEMS:')
        for p in problems:
            print('  ', p)
        sys.exit(1)
    print('verification passed: no excluded path, picture, audio, disc or state file, or blob over 1 MB in any revision')


if __name__ == '__main__':
    main()
