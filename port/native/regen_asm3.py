#!/usr/bin/env python3
"""Regenerate port/native/replacements/battle_asm3.c from the retail BATTLE overlay on your own disc.

The file is a mechanical translation of the game's machine code, so it is not part of the public export of this repository
(see PUBLISHING.md); this script rebuilds it, byte for byte, with port/tools/mips2c.py. Needs fft_decomp/build/extracted (run the
decomp's extraction first, see HOW-TO-PLAY.md).
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'port' / 'native' / 'replacements' / 'battle_asm3.c'
HEADER = ('/* Native C versions of the BATTLE map polygon queuers (hand-assembled GTE routines, kind: handwritten), translated mechanically from the '
          'retail machine code by port/tools/mips2c.py. The tool also documents its own limits; regenerate with:  python port/tools/mips2c.py --out '
          'port/native/replacements/battle_asm3.c battle_map_queue_textured_triangles battle_map_queue_textured_quads '
          'battle_map_queue_untextured_triangles battle_map_queue_untextured_quads */')
NAMES = ['battle_map_queue_textured_triangles', 'battle_map_queue_textured_quads',
         'battle_map_queue_untextured_triangles', 'battle_map_queue_untextured_quads']

if __name__ == '__main__':
    cmd = [sys.executable, str(ROOT / 'port' / 'tools' / 'mips2c.py'), '--out', str(OUT), '--header', HEADER] + NAMES
    sys.exit(subprocess.call(cmd))
