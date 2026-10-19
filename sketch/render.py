#!/usr/bin/env python3
"""fluent_audio sketch — offline render of the look presets, so the direction can be heard.

NOT the library. A throwaway numpy prototype of a few v1 nodes (docs/design/fluent_audio.ja.md §4) with the
same parameter names and defaults the C++ core is meant to have. Recursive parts run in 128-sample blocks
like the real-time core will (the FDN reads its delay lines a block at a time, which needs block <= the
shortest line).

  python3 sketch/render.py <voice.(mp3|wav)> <out_dir> [preset ...]     # presets: dry kepler fluent kitt gargantua

Writes <out_dir>/faudio-<preset>.mp3 (48 kHz stereo, loudness-matched to dry so only the colour differs).
Needs numpy + scipy and ffmpeg on PATH.
"""
import subprocess
import sys

import numpy as np
from scipy.signal import lfilter

SR = 48000
BLOCK = 128


# ---------------------------------------------------------------- io
def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    x = np.frombuffer(raw, np.float32).astype(np.float64)
    return np.concatenate([x, np.zeros(int(SR * 2.5))])          # room for tails


def save(path, st):
    pcm = np.clip(st.T, -1, 1).astype(np.float32).tobytes()
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", str(SR), "-ac", "2", "-i", "-",
                    "-c:a", "libmp3lame", "-b:a", "160k", path], input=pcm, check=True)


# ---------------------------------------------------------------- nodes
def db(g):
    return 10 ** (g / 20)


def biquad(kind, f, q=0.707, gain_db=0.0):
    """RBJ cookbook coefficients (b, a)."""
    A = 10 ** (gain_db / 40)
    w = 2 * np.pi * f / SR
    c, s = np.cos(w), np.sin(w)
    al = s / (2 * q)
    if kind == "hp":
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]; a = [1 + al, -2 * c, 1 - al]
    elif kind == "lp":
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]; a = [1 + al, -2 * c, 1 - al]
    elif kind == "peak":
        b = [1 + al * A, -2 * c, 1 - al * A]; a = [1 + al / A, -2 * c, 1 - al / A]
    elif kind in ("lowshelf", "highshelf"):
        sq = 2 * np.sqrt(A) * al
        if kind == "lowshelf":
            b = [A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq)]
            a = [(A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq]
        else:
            b = [A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq)]
            a = [(A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq]
    else:
        raise ValueError(kind)
    return np.array(b) / a[0], np.array(a) / a[0]


def eq(x, bands):
    for band in bands:
        b, a = biquad(*band)
        x = lfilter(b, a, x, axis=-1)
    return x


def env_follow(level, attack_ms, release_ms):
    """Peak-ish envelope with separate attack/release (one pole each)."""
    ga, gr = np.exp(-1 / (SR * attack_ms / 1000)), np.exp(-1 / (SR * release_ms / 1000))
    out = np.empty_like(level); e = 0.0
    for i, v in enumerate(level):
        g = ga if v > e else gr
        e = g * e + (1 - g) * v
        out[i] = e
    return out


def compressor(x, threshold=-20, ratio=2.0, attack_ms=8, release_ms=120, knee=6, makeup=0.0, key=None):
    """Feed-forward, soft knee, log-domain gain computer. `key` = sidechain (defaults to x)."""
    k = x if key is None else key
    lvl = 20 * np.log10(env_follow(np.abs(k), attack_ms, release_ms) + 1e-9)
    over = lvl - threshold
    gr = np.where(over <= -knee / 2, 0.0,
                  np.where(over >= knee / 2, over * (1 - 1 / ratio),
                           (1 - 1 / ratio) * (over + knee / 2) ** 2 / (2 * knee)))
    return x * db(-gr + makeup)


def deesser(x, freq=6500, threshold=-30, ratio=3.0):
    """Split-band: compress only the band above `freq`, keyed by that band."""
    b, a = biquad("hp", freq)
    hi = lfilter(b, a, x)
    return x - hi + compressor(hi, threshold, ratio, 1, 60, 6, key=hi)


def exciter(x, freq=3500, drive=2.0, mix=0.12):
    """Harmonics from the top band only (tanh), high-passed again so nothing muddy is added."""
    b, a = biquad("hp", freq)
    h = np.tanh(drive * lfilter(b, a, x)) / drive
    return x + mix * lfilter(b, a, h)


def saturate(x, drive=1.5, mix=1.0):
    """Gentle tanh, level-compensated."""
    return (1 - mix) * x + mix * np.tanh(drive * x) / np.tanh(drive)


def frac_read(buf, pos):
    i = np.floor(pos).astype(int); f = pos - i
    return buf[np.clip(i, 0, len(buf) - 1)] * (1 - f) + buf[np.clip(i + 1, 0, len(buf) - 1)] * f


def chorus(x, voices=2, rate=0.25, depth_ms=2.5, base_ms=12, mix=0.3, spread=1.0):
    """Modulated delays, voices panned L/R. Returns stereo."""
    n = np.arange(len(x)); out = np.zeros((2, len(x)))
    for v in range(voices):
        ph = 2 * np.pi * (rate * (1 + 0.13 * v) * n / SR + v / voices)
        d = (base_ms + depth_ms * (0.5 + 0.5 * np.sin(ph))) * SR / 1000
        y = frac_read(x, n - d)
        pan = (v / max(1, voices - 1) - 0.5) * spread     # -0.5..0.5
        out[0] += y * np.sqrt(0.5 - pan); out[1] += y * np.sqrt(0.5 + pan)
    out /= voices
    return np.vstack([x, x]) * (1 - mix * 0.5) + mix * out


def widen(st, haas_ms=0.0, width=1.0):
    """M/S width (mono-safe) + optional tiny Haas delay on the side only."""
    m, s = (st[0] + st[1]) / 2, (st[0] - st[1]) / 2
    s = s * width
    if haas_ms:
        d = int(haas_ms * SR / 1000)
        s = s + 0.3 * np.concatenate([np.zeros(d), m[:-d]]) - 0.3 * m      # side-only: mono sum unchanged
    return np.vstack([m + s, m - s])


def pitch_shift(x, semitones=12.0, window_ms=60):
    """Two-tap delay-line shifter with sin^2 crossfade (Doppler). Cheap, smeary — fine inside a reverb."""
    r = 2 ** (semitones / 12); w = window_ms * SR / 1000
    n = np.arange(len(x))
    ph = ((1 - r) * n / w) % 1.0
    out = np.zeros_like(x)
    for off in (0.0, 0.5):
        p = (ph + off) % 1.0
        out += frac_read(x, n - p * w) * np.sin(np.pi * p) ** 2
    return out


def fdn_reverb(x, decay_s=1.2, size=1.0, damp_hz=6000, predelay_ms=10, mod=0.0):
    """8-line FDN, Householder feedback, one-pole damping per line. Block-processed (BLOCK=128). Stereo out."""
    base = np.array([1031, 1327, 1523, 1801, 2053, 2311, 2633, 2897])
    L = np.maximum((base * size * SR / 48000).astype(int), BLOCK + 1)
    g = 10 ** (-3 * L / (decay_s * SR))                      # per-line gain for RT60
    bd, ad = biquad("lp", damp_hz, 0.5)
    pd = int(predelay_ms * SR / 1000)
    x = np.concatenate([np.zeros(pd), x])[:len(x)]
    N = len(L); bufs = [np.zeros(l) for l in L]; w = [0] * N; zi = [np.zeros(2) for _ in range(N)]
    nb = len(x) // BLOCK; out = np.zeros((2, nb * BLOCK))
    sgnL = np.array([1, -1, 1, -1, 1, -1, 1, -1]) * 0.5; sgnR = np.array([1, 1, -1, -1, 1, 1, -1, -1]) * 0.5
    for bi in range(nb):
        s = slice(bi * BLOCK, (bi + 1) * BLOCK)
        ys = np.empty((N, BLOCK))
        for i in range(N):
            idx = (w[i] + np.arange(BLOCK)) % L[i]              # oldest samples = delay L
            y, zi[i] = lfilter(bd, ad, bufs[i][idx], zi=zi[i])
            ys[i] = y * g[i]
        fb = ys - (2 / N) * ys.sum(0)                           # Householder: I - 2/N 11^T
        for i in range(N):
            idx = (w[i] + np.arange(BLOCK)) % L[i]
            bufs[i][idx] = x[s] * 0.35 + fb[i]
            w[i] = (w[i] + BLOCK) % L[i]
        out[0, s] = sgnL @ ys; out[1, s] = sgnR @ ys
    pad = len(x) - nb * BLOCK
    return np.pad(out, ((0, 0), (0, pad)))


def ringmod(x, freq=30, mix=0.08):
    n = np.arange(len(x))
    return x * (1 - mix + mix * np.sin(2 * np.pi * freq * n / SR))


def limiter(st, ceiling_db=-1.0):
    """Stand-in for the true-peak limiter: fast compressor on the louder channel, then a hard ceiling."""
    key = np.max(np.abs(st), axis=0)
    c = db(ceiling_db)
    lvl = env_follow(key, 0.5, 80)
    gain = np.minimum(1.0, c / np.maximum(lvl, 1e-9))
    return np.clip(st * gain, -c, c)


def mono(x):
    return np.vstack([x, x])


# ---------------------------------------------------------------- presets (mirror presets/*.yaml)
def clean(x):
    """Shared front end of every look: rumble cut, gentle levelling, de-ess."""
    x = eq(x, [("hp", 80, 0.7)])
    x = compressor(x, threshold=-24, ratio=2.0, attack_ms=10, release_ms=150, makeup=2)
    return deesser(x, 6500, -32, 3)


def p_dry(x):
    return mono(x)


def p_kepler(x):
    x = clean(x)
    x = eq(x, [("peak", 3200, 1.0, 1.5), ("highshelf", 10000, 0.7, 2.5)])     # presence + air
    x = exciter(x, 4500, 2.0, 0.08)
    st = mono(x) + 0.05 * fdn_reverb(x, decay_s=0.5, size=0.45, damp_hz=7000, predelay_ms=6)
    return st


def p_fluent(x):
    x = clean(x)
    x = eq(x, [("lowshelf", 180, 0.7, -1.5), ("peak", 2800, 0.9, 1.5), ("highshelf", 9000, 0.7, 3)])
    x = exciter(x, 5000, 2.5, 0.10)
    st = chorus(x, voices=3, rate=0.18, depth_ms=1.8, base_ms=9, mix=0.28)     # soft liquid double
    st = widen(st, width=1.3)
    wet = fdn_reverb(x, decay_s=1.6, size=0.8, damp_hz=8000, predelay_ms=18)
    shim = fdn_reverb(pitch_shift(eq(x, [("hp", 600, 0.7)]), 12), decay_s=2.8, size=1.1, damp_hz=9000,
                      predelay_ms=40)
    return st + 0.10 * wet + 0.22 * shim


def p_kitt(x):
    x = clean(x)
    x = eq(x, [("hp", 220, 0.8), ("lp", 6000, 0.8), ("peak", 1400, 0.9, 3.0), ("peak", 350, 1.0, -2)])
    x = saturate(x * 1.4, drive=1.8, mix=0.6)
    x = ringmod(x, 55, 0.05)                                     # a faint electronic grain, not a dalek
    x = compressor(x, threshold=-22, ratio=3, attack_ms=3, release_ms=80, makeup=2)
    st = mono(x) + 0.06 * fdn_reverb(x, decay_s=0.35, size=0.3, damp_hz=5000, predelay_ms=3)
    return widen(st, haas_ms=0.0, width=1.0)


def p_gargantua(x):
    x = clean(x)
    x = eq(x, [("lowshelf", 160, 0.7, 2.0), ("peak", 3000, 1.0, 1.0), ("highshelf", 8000, 0.7, -1.0)])
    dry = mono(x)
    space = fdn_reverb(x, decay_s=5.5, size=1.6, damp_hz=3500, predelay_ms=60)
    shim = fdn_reverb(pitch_shift(eq(x, [("hp", 400, 0.7)]), -12), decay_s=6.0, size=1.8, damp_hz=2500,
                      predelay_ms=90)
    return dry + 0.14 * space + 0.12 * shim


PRESETS = {"dry": p_dry, "kepler": p_kepler, "fluent": p_fluent, "kitt": p_kitt, "gargantua": p_gargantua}


def loud(st):
    """Gated RMS of the mid signal (a crude stand-in for LUFS)."""
    m = (st[0] + st[1]) / 2
    fr = m[: len(m) // 2400 * 2400].reshape(-1, 2400)
    r = np.sqrt((fr ** 2).mean(1))
    return np.sqrt((r[r > r.max() * 0.1] ** 2).mean())


def main():
    src, out_dir = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or list(PRESETS)
    x = load(src)
    ref = loud(mono(x))
    for name in names:
        st = PRESETS[name](x.copy())
        st *= ref / loud(st)
        st = limiter(st, -1.0)
        # trim trailing silence (keep tails)
        e = np.max(np.abs(st), 0); last = np.nonzero(e > 1e-4)[0]
        st = st[:, : (last[-1] + int(0.1 * SR)) if len(last) else st.shape[1]]
        path = f"{out_dir}/faudio-{name}.mp3"
        save(path, st)
        print(f"{name:10s} -> {path}  peak {20 * np.log10(np.abs(st).max()):.1f} dBFS")


if __name__ == "__main__":
    main()
