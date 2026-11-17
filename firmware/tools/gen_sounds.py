"""Generate Xiaophone's sounds (22.05 kHz mono 16-bit WAV) into ../data/.

chime.wav   - soft 3-note music-box arpeggio (C6, E6, G6)
crackle.wav - ~1 s of quiet vinyl crackle (filtered hiss + random pops)
Run: python tools/gen_sounds.py   (stdlib only)
"""
import math, random, struct, wave, os

SR = 22050
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")


def write(name, samples):
    with wave.open(os.path.join(OUT, name), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767)))) for s in samples))
    print(name, len(samples) / SR, "s")


def chime():
    notes = [1046.5, 1318.5, 1568.0]          # C6 E6 G6
    n = int(SR * 1.6)
    out = [0.0] * n
    for k, f in enumerate(notes):
        start = int(SR * 0.18 * k)
        for i in range(n - start):
            t = i / SR
            env = min(1.0, t / 0.004) * math.exp(-t * 4.0)       # tine pluck
            s = math.sin(2 * math.pi * f * t) + 0.25 * math.sin(2 * math.pi * 2 * f * t) * math.exp(-t * 9)
            out[start + i] += 0.22 * env * s
    return out


def crackle():
    random.seed(7)
    n = int(SR * 1.0)
    out, lp = [], 0.0
    for i in range(n):
        lp += 0.15 * (random.uniform(-1, 1) - lp)               # soft hiss
        out.append(0.05 * lp)
    for _ in range(28):                                          # pops
        p, amp, ln = random.randrange(n - 80), random.uniform(0.08, 0.3), random.randint(15, 60)
        sign = random.choice((-1, 1))
        for j in range(ln):
            out[p + j] += sign * amp * math.exp(-j / (ln / 4)) * (1 if j % 2 == 0 else -0.6)
    fade = int(SR * 0.1)
    for i in range(fade):
        out[i] *= i / fade; out[-1 - i] *= i / fade
    return out


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    write("chime.wav", chime())
    write("crackle.wav", crackle())
