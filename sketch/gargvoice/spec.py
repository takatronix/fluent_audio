import sys, numpy as np, scipy.io.wavfile as wf, scipy.signal as ss, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
names = sys.argv[1:]; fig, ax = plt.subplots(len(names), 1, figsize=(16, 2.6 * len(names)))
for a, n in zip(np.atleast_1d(ax), names):
    sr, x = wf.read(f'/home/vegetalia/fluent_audio/sketch/gargvoice/out/{n}.wav'); x = x.astype(float).mean(1) if x.ndim > 1 else x.astype(float)
    f, t, S = ss.spectrogram(x, sr, nperseg=4096, noverlap=3072)
    a.pcolormesh(t, f, 10 * np.log10(S + 1e-3), vmin=np.percentile(10*np.log10(S+1e-3), 50), shading='auto', cmap='magma'); a.set_ylim(0, 2500); a.set_title(n)
plt.tight_layout(); plt.savefig('/tmp/gv_spec.png', dpi=60)
