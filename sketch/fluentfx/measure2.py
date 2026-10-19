# Per-frame comb ripple (mono sum and left ear), tail after each line, spectral centroid, S/M width.
# usage: measure2.py dry.wav wet.wav...
import sys, numpy as np, scipy.io.wavfile as wf, scipy.signal as ss
def load(p):
    sr, x = wf.read(p); x = x.astype(np.float64)
    if np.abs(x).max() > 2: x /= 32768
    if x.ndim == 1: x = np.stack([x, x], 1)
    return sr, x
sr, d = load(sys.argv[1]); dm = d.mean(1)
env = lambda y: np.sqrt(ss.lfilter([1], [1, -0.995], y**2) * 0.005 + 1e-20)
ed = env(dm); on = ed > ed.max() * 10**(-35/20)
ends = [i for i in range(1, len(on)) if on[i-1] and not on[i] and not on[i:i+int(0.25*sr)].any()]
f, t, Sd = ss.stft(dm, sr, nperseg=2048, noverlap=1536)
voiced = (np.abs(Sd)**2).sum(0) > np.percentile((np.abs(Sd)**2).sum(0), 60)
sel = (f > 800) & (f < 8000)
def ripple(S):
    r = 10*np.log10(np.abs(S[sel][:, voiced])**2+1e-20) - 10*np.log10(np.abs(Sd[sel][:, voiced])**2+1e-20)
    sm = ss.savgol_filter(r, 31, 2, axis=0)
    return np.median(np.std(r - sm, axis=0))
print(f"{'name':18s} ripM  ripL  cent   S/M   tail@0.1  @0.3  @0.6 (dB re speech)")
for p in sys.argv[1:]:
    _, x = load(p); n = min(len(x), len(d)); x = x[:n]; m = x.mean(1)
    _, _, Sm = ss.stft(m, sr, nperseg=2048, noverlap=1536); _, _, Sl = ss.stft(x[:, 0], sr, nperseg=2048, noverlap=1536)
    P = (np.abs(Sm)**2)[:, voiced].mean(1); cent = (f*P).sum()/P.sum()
    sp = np.sqrt((m[on[:n]]**2).mean())
    tl = []
    for dt in (0.1, 0.3, 0.6):
        v = [np.sqrt((m[e+int((dt-0.05)*sr):e+int((dt+0.05)*sr)]**2).mean()) for e in ends if e+int((dt+0.05)*sr) < n]
        tl.append(20*np.log10(np.mean(v)/sp+1e-12))
    s = 0.5*(x[:, 0]-x[:, 1]); mm = 0.5*(x[:, 0]+x[:, 1])
    print(f"{p.split('/')[-1][:18]:18s} {ripple(Sm):4.2f}  {ripple(Sl):4.2f}  {cent:5.0f}  {10*np.log10((s**2).sum()/(mm**2).sum()+1e-20):5.1f}  " + "  ".join(f"{v:6.1f}" for v in tl))
