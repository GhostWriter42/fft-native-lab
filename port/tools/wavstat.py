#!/usr/bin/env python3
"""wavstat.py RAW [OUT.wav] -- level per 2 seconds of a raw dump (signed 16 bit stereo, 44.1 kHz) written by the driver's `audiodump`, and an optional WAV copy."""
import array
import math
import sys
import wave

a = array.array('h')
a.frombytes(open(sys.argv[1], 'rb').read())
n = len(a) // 2
step = 44100 * 2
out = []
for i in range(0, n, step):
    seg = a[2 * i:2 * (i + step):2]
    if not seg:
        break
    out.append('%ds:rms %d peak %d' % (i // 44100, math.sqrt(sum(x * x for x in seg) / len(seg)), max(abs(x) for x in seg)))
print('%.1f seconds' % (n / 44100))
print('\n'.join(out))
if len(sys.argv) > 2:
    w = wave.open(sys.argv[2], 'wb')
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(44100)
    w.writeframes(a.tobytes())
    w.close()
