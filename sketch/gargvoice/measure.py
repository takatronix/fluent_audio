# Per candidate (the server-like renders in out/<name>_l<i>.wav): spectral centroid and >5 kHz share during the
# speech, S/M width, how long the tail rings after the last word (to -20/-40 dB re speech), click score.
import sys, numpy as np, scipy.io.wavfile as wf, scipy.signal as ss
O = '/home/vegetalia/fluent_audio/sketch/gargvoice/'
def ld(p):
    sr, x = wf.read(p); x = x.astype(float) / 32768; return sr, (x if x.ndim > 1 else np.stack([x, x], 1))
print(f"{'name':10s} cent  >5k%  <200%  S/M   tail-20 tail-40  click")
for n in sys.argv[1:]:
    r = []
    for i in (1, 2, 3, 4):
        srd, d = wf.read(O + f'dry/l{i}.wav'); sr, x = ld(O + f'out/{n}_l{i}.wav'); ns = int(len(d) / srd * sr)
        m = x.mean(1); sp = m[:ns]; f, P = ss.welch(sp, sr, nperseg=4096)
        e = np.sqrt(ss.lfilter([1], [1, -0.999], m ** 2) * 0.001 + 1e-20); ref = np.sqrt((sp ** 2).mean())
        t20 = (np.nonzero(e > ref * 0.1)[0].max() - ns) / sr; t40 = (np.nonzero(e > ref * 0.01)[0].max() - ns) / sr
        hp = ss.lfilter(*ss.butter(2, 8000 / (sr / 2), 'high'), m); click = np.abs(hp).max() / (np.sqrt((hp[:ns] ** 2).mean()) + 1e-9)
        s = 0.5 * (x[:, 0] - x[:, 1]); mm = 0.5 * (x[:, 0] + x[:, 1])
        r.append(((f * P).sum() / P.sum(), 100 * P[f > 5000].sum() / P.sum(), 100 * P[f < 200].sum() / P.sum(), 20 * np.log10(np.sqrt((s ** 2).mean() / (mm ** 2).mean())), t20, t40, click))
    print(f'{n:10s}' + ' '.join(f'{v:5.1f}' if j != 0 else f'{v:5.0f}' for j, v in enumerate(np.mean(r, 0))))
