# Stems of one gargantua preset on one line: full, voice path only (organ send 0), organ only (other sends 0, dry 0).
# Prints organ/voice RMS during speech, organ level after the speech in 0.5 s steps, organ spectral peaks.
# usage: stems.py <name> [line] [--param k=v ...]
import subprocess, sys, numpy as np, scipy.io.wavfile as wf
name = sys.argv[1]; line = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else '4'
extra = sys.argv[3:] if len(sys.argv) > 3 else []
FA = '/home/vegetalia/fluent_audio/build/fa_render'; P = f'/home/vegetalia/fluent_audio/presets/gargantua_{name}.yaml'
D = f'/home/vegetalia/fluent_audio/sketch/gargvoice/dry/l{line}.wav'
desc = subprocess.run([FA, '--describe', P], capture_output=True, text=True).stdout
pnames = [l.split()[0] for l in desc.splitlines() if l.strip().startswith(f'gargantua_{name}.')]
sends = [p for p in pnames if '.send' in p]
def render(params, out):
    a = [FA, P, '-', D, out, '--tail', '4', '--no-trim']
    for k, v in params.items(): a += ['--param', f'{k}={v}']
    subprocess.run(a + extra, check=True, capture_output=True)
    sr, x = wf.read(out); return sr, x.astype(np.float64) / (32768 if x.dtype == np.int16 else 1)
organ_send = [s for s in sends if s.endswith('send1.gain')][0]
sr, full = render({}, '/tmp/gv_full.wav')
_, voice = render({organ_send: 0}, '/tmp/gv_voice.wav')
org = full - voice
srd, d = wf.read(D); n_sp = int(len(d) / srd * sr)
m = lambda y: y.mean(1)
rms = lambda y: np.sqrt((y ** 2).mean() + 1e-20)
db = lambda r: 20 * np.log10(r + 1e-12)
vs = rms(m(voice)[:n_sp]); print(f'{name} l{line}: organ/voice during speech {db(rms(m(org)[:n_sp]) / vs):+.1f} dB, full peak {db(np.abs(full).max()):.1f} dBFS')
print('  after speech (dB re voice RMS), full / organ-path:', ' '.join(f'{db(rms(m(full)[n_sp+int(t*sr):n_sp+int((t+.5)*sr)])/vs):+.0f}/{db(rms(m(org)[n_sp+int(t*sr):n_sp+int((t+.5)*sr)])/vs):+.0f}' for t in np.arange(0, 3.6, 0.5)))
seg = m(org)[:n_sp]; F = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); f = np.fft.rfftfreq(len(seg), 1 / sr)
idx = [i for i in range(1, len(F) - 1) if F[i] > F[i - 1] and F[i] > F[i + 1] and f[i] < 2000]
idx = sorted(idx, key=lambda i: -F[i])[:12]
print('  organ peaks Hz:', ' '.join(f'{f[i]:.0f}' for i in sorted(idx)))
s = 0.5 * (full[:, 0] - full[:, 1]); mm = 0.5 * (full[:, 0] + full[:, 1]); print(f'  S/M {db(rms(s) / rms(mm)):+.1f} dB; max |diff| {np.abs(np.diff(full[:, 0])).max():.3f}')
