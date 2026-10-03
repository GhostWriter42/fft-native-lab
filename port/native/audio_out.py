#!/usr/bin/env python3
"""audio_out.py -- play the 44.1 kHz stereo 16-bit chunks the game streams (one per frame) through the Windows waveOut API (winmm.dll via ctypes; no packages needed).

  out = AudioOut()          # None-like object that ignores everything when the device cannot be opened
  out.write(chunk_bytes)    # queue one chunk; when every buffer is still playing (the game runs faster than real time) the chunk is dropped
  out.close()

`python audio_out.py` plays a two-second test tone. The game's own sound is produced by the software SPU model (hle/spu.c)."""
import ctypes
import sys
from ctypes import wintypes as wt

WAVE_MAPPER = 0xFFFFFFFF
WHDR_DONE = 0x1
WHDR_PREPARED = 0x2


class WAVEFORMATEX(ctypes.Structure):
    _fields_ = [('wFormatTag', wt.WORD), ('nChannels', wt.WORD), ('nSamplesPerSec', wt.DWORD), ('nAvgBytesPerSec', wt.DWORD),
                ('nBlockAlign', wt.WORD), ('wBitsPerSample', wt.WORD), ('cbSize', wt.WORD)]


class WAVEHDR(ctypes.Structure):
    _fields_ = [('lpData', ctypes.c_void_p), ('dwBufferLength', wt.DWORD), ('dwBytesRecorded', wt.DWORD), ('dwUser', ctypes.c_size_t), ('dwFlags', wt.DWORD),
                ('dwLoops', wt.DWORD), ('lpNext', ctypes.c_void_p), ('reserved', ctypes.c_size_t)]


class AudioOut:
    def __init__(self, rate=44100, buffers=10, chunk=735 * 4):
        self.ok = False
        self.dropped = 0
        self.written = 0
        if sys.platform != 'win32':
            return
        try:
            self.winmm = ctypes.WinDLL('winmm')
            fmt = WAVEFORMATEX(1, 2, rate, rate * 4, 4, 16, 0)
            self.hwo = wt.HANDLE()
            if self.winmm.waveOutOpen(ctypes.byref(self.hwo), WAVE_MAPPER, ctypes.byref(fmt), 0, 0, 0) != 0:
                return
            self.bufs = [ctypes.create_string_buffer(chunk) for _ in range(buffers)]
            self.hdrs = [WAVEHDR(ctypes.cast(b, ctypes.c_void_p), chunk, 0, 0, 0, 0, None, 0) for b in self.bufs]
            self.chunk = chunk
            self.ok = True
            for _ in range(2):                                   # a little silence first: the queue never starts empty
                self.write(b'\0' * chunk)
        except (OSError, AttributeError):
            self.ok = False

    def write(self, data):
        if not self.ok or not data:
            return
        for i, h in enumerate(self.hdrs):
            if h.dwFlags == 0 or (h.dwFlags & WHDR_DONE):
                if h.dwFlags & WHDR_PREPARED:
                    self.winmm.waveOutUnprepareHeader(self.hwo, ctypes.byref(h), ctypes.sizeof(h))
                n = min(len(data), self.chunk)
                ctypes.memmove(self.bufs[i], data, n)
                h.dwBufferLength = n
                h.dwFlags = 0
                self.winmm.waveOutPrepareHeader(self.hwo, ctypes.byref(h), ctypes.sizeof(h))
                self.winmm.waveOutWrite(self.hwo, ctypes.byref(h), ctypes.sizeof(h))
                self.written += 1
                return
        self.dropped += 1

    def close(self):
        if self.ok:
            self.winmm.waveOutReset(self.hwo)
            for h in self.hdrs:
                if h.dwFlags & WHDR_PREPARED:
                    self.winmm.waveOutUnprepareHeader(self.hwo, ctypes.byref(h), ctypes.sizeof(h))
            self.winmm.waveOutClose(self.hwo)
            self.ok = False


if __name__ == '__main__':
    import math
    import struct
    import time
    a = AudioOut()
    print('device opened:', a.ok)
    t = 0
    for _ in range(120):
        chunk = b''.join(struct.pack('<hh', *(int(6000 * math.sin(2 * math.pi * 440 * (t + i) / 44100)),) * 2) for i in range(735))
        t += 735
        a.write(chunk)
        time.sleep(1 / 60)
    time.sleep(0.3)
    print('written', a.written, 'dropped', a.dropped)
    a.close()
