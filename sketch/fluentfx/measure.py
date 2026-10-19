# Listen-by-numbers: spectrum bands, harshness, stereo width, tail, comb-ness. usage: measure.py dry.wav x.wav...
import sys, numpy as np, scipy.io.wavfile as wf, scipy.signal as ss
def load(p):
    sr, x = wf.read(p); x = x.astype(np.float64)
    if x.dtype.kind == 'i' or np.abs(x).max() > 2: x /= 32768
    if x.ndim == 1: x = np.stack([x, x], 1)
    return sr, x
sr, d = load(sys.argv[1]); dm = d.mean(1)
bands = [(80,250,'low'),(250,800,'lowmid'),(800,2500,'mid'),(2500,5000,'pres'),(5000,9000,'sib'),(9000,16000,'air')]
def bandspec(m):
    f, P = ss.welch(m, sr, nperseg=4096); tot = P[(f>80)&(f<16000)].sum()
    return f, P, [10*np.log10(P[(f>=a)&(f<b)].sum()/tot+1e-20) for a,b,_ in bands]
fd, Pd, bd = bandspec(dm)
print(f"{'name':16s}" + ''.join(f"{n:>8s}" for *_,n in bands) + "   width  corr  tail-20dB  rough  flat")
for p in sys.argv[1:]:
    _, x = load(p); n = min(len(x), len(d)) ; m = x.mean(1); s = 0.5*(x[:,0]-x[:,1])
    f, P, b = bandspec(m)
    width = 10*np.log10((s**2).sum()/((0.5*(x[:,0]+x[:,1]))**2).sum()+1e-20)
    corr = np.corrcoef(x[:,0], x[:,1])[0,1]
    # tail: time after the dry ends (last sample above -50 dBFS env) until wet env falls 20 dB below its speech rms
    env = lambda y: np.sqrt(ss.lfilter([1],[1,-0.999],y**2)*0.001+1e-20)
    ed = env(dm); ew = env(m)
    endd = np.nonzero(ed > 10**(-50/20))[0][-1]
    ref = np.sqrt((m[:endd]**2).mean())
    endw = np.nonzero(ew > ref*0.1)[0][-1]
    tail = (endw-endd)/sr
    # roughness: spectral ripple (comb) of wet relative to dry in 1-8 kHz: std of the dB diff after 1/3-oct smoothing
    sel = (f>1000)&(f<8000)
    r = 10*np.log10(P[sel]+1e-20) - 10*np.log10(Pd[sel]+1e-20)
    sm = np.convolve(r, np.ones(15)/15, 'same')
    rough = np.std(r - sm)
    flat = np.exp(np.mean(np.log(P[sel]+1e-30)))/np.mean(P[sel])
    print(f"{p.split('/')[-1][:16]:16s}" + ''.join(f"{v-bd[i]:+8.1f}" if p!=sys.argv[1] else f"{v:8.1f}" for i,v in enumerate(b)) +
          f"  {width:6.1f} {corr:5.2f}   {tail:6.2f}   {rough:5.2f} {flat:5.3f}")
