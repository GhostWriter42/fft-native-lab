"""Opens the Windows audio device and queues two seconds of SILENCE (no sound is played): checks that AudioOut works on this machine."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audio_out import AudioOut

a = AudioOut()
print('device opened:', a.ok)
for _ in range(120):
    a.write(b'\0' * (735 * 4))
    time.sleep(1 / 60)
time.sleep(0.3)
print('chunks written', a.written, 'dropped', a.dropped)
a.close()
