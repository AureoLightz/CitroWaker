#!/usr/bin/env python3
"""Generate CitroWaker's original HOME highlight. No sampled/game audio."""
import math
import struct
import wave
from pathlib import Path

RATE = 32000
DURATION = 2.7
# A brief, original rising bell gesture; no quotation of a game melody.
NOTES = ((0.04, 587.3295, -0.3), (0.24, 880.0, 0.2), (0.48, 1318.5102, 0.05))

def main():
    samples = []
    for i in range(round(RATE * DURATION)):
        t = i / RATE
        channels = [0.0, 0.0]
        for start, freq, pan in NOTES:
            age = t - start
            if age < 0:
                continue
            envelope = (1 - math.exp(-age / 0.009)) * math.exp(-age / 0.43)
            tone = sum(gain * math.sin(math.tau * freq * ratio * age)
                       for ratio, gain in ((1, 1), (2, .22), (3.01, .07)))
            for c, balance in enumerate(((1-pan)/2, (1+pan)/2)):
                channels[c] += .25 * envelope * tone * math.sqrt(balance)
        fade = min(1.0, max(0.0, (DURATION - t) / .15))
        samples.extend(round(v * fade * 32767) for v in channels)
    assert max(abs(v) for v in samples) < 32767
    target = Path(__file__).with_name('citrowaker-highlight.wav')
    with wave.open(str(target), 'wb') as wav:
        wav.setparams((2, 2, RATE, 0, 'NONE', 'not compressed'))
        wav.writeframes(struct.pack(f'<{len(samples)}h', *samples))
    print(target)

if __name__ == '__main__':
    main()
