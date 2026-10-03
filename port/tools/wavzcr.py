import array
import sys
a = array.array('h')
a.frombytes(open(sys.argv[1], 'rb').read())
left = a[0::2]
w = 44100 // 2
for i in range(0, len(left) - w, w * 4):
    seg = left[i:i + w]
    zc = sum(1 for k in range(1, len(seg)) if (seg[k - 1] < 0) != (seg[k] < 0))
    clip = sum(1 for x in seg if abs(x) >= 32767)
    print('%6.1fs  zero crossings/s %5d  clipped samples %5d' % (i / 44100, zc * 2, clip))
