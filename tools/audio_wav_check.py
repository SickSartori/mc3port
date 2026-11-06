"""Facts about a mixer dump: level, clipping, clicks, hiss. Usage: wav_check.py audio.wav [start_s]"""
import sys, wave
import numpy as np

path = sys.argv[1]
start = float(sys.argv[2]) if len(sys.argv) > 2 else 0
with wave.open(path) as w:
    rate, ch = w.getframerate(), w.getnchannels()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, ch) / 32768.0
x = x[int(start * rate):]
mono = x.mean(axis=1)
n = len(mono)
print(f"{path}: {n / rate:.1f} s from {start:.0f} s, {rate} Hz, {ch} ch")
assert rate == 48000 and ch == 2

# Level per second: peak and RMS in dBFS.
sec = n // rate
peaks = np.abs(x[: sec * rate]).reshape(sec, rate * ch).max(axis=1)
rms = np.sqrt((x[: sec * rate] ** 2).reshape(sec, rate * ch).mean(axis=1))
db = lambda v: 20 * np.log10(np.maximum(v, 1e-9))
print(f"peak overall {db(peaks.max()):.1f} dBFS; seconds with peak >= -1 dBFS: {(db(peaks) >= -1).sum()}/{sec}")
print(f"RMS median {np.median(db(rms)):.1f} dBFS, loudest second {db(rms).max():.1f} dBFS, quietest {db(rms).min():.1f}")
print("crest factor (peak/RMS) median %.1f dB: under ~10 dB means squashed by a limiter" % np.median(db(peaks) - db(rms)))

# Clipping: runs of samples pinned at the ceiling.
ceiling = np.abs(x) >= 0.89
print(f"samples at the limiter ceiling (>= 0.89): {ceiling.sum()} ({100 * ceiling.mean():.2f}%)")

# Clicks: sample-to-sample jumps far bigger than the signal around them.
d = np.abs(np.diff(mono))
big = np.flatnonzero(d > 0.25)
print(f"jumps > 0.25 between samples: {len(big)}")
if len(big):
    for i in big[:12]:
        print(f"  at {i / rate:7.3f} s: {mono[i]:+.2f} -> {mono[i + 1]:+.2f}")

# Hiss / imaging: energy above 16 kHz vs 1-8 kHz, over 1 s windows (a car game mix has little real content up there).
win = rate
ratios = []
for s in range(0, n - win, win):
    spec = np.abs(np.fft.rfft(mono[s : s + win] * np.hanning(win))) ** 2
    f = np.fft.rfftfreq(win, 1 / rate)
    high = spec[f >= 16000].sum()
    mid = spec[(f >= 1000) & (f < 8000)].sum()
    ratios.append(10 * np.log10(max(high, 1e-12) / max(mid, 1e-12)))
ratios = np.array(ratios)
print(f"energy 16-24 kHz vs 1-8 kHz: median {np.median(ratios):.1f} dB, max {ratios.max():.1f} dB (clean music/engines: under -30 dB)")

# Silence and dropouts: short gaps inside otherwise loud audio.
frame = rate // 100  # 10 ms, the mixer chunk
f_rms = np.sqrt((mono[: (n // frame) * frame] ** 2).reshape(-1, frame).mean(axis=1))
loud = f_rms > 0.02
gaps = [(i, ) for i in range(1, len(f_rms) - 1) if loud[i - 1] and loud[i + 1] and f_rms[i] < 0.002]
print(f"10 ms chunks near-silent between loud chunks (dropouts): {len(gaps)}")
for (i,) in gaps[:10]:
    print(f"  at {i * frame / rate:7.3f} s")
