// native ↔ wasm check (design §9): render the same WAV through the same look with fluent_audio.wasm (as the
// worklet does: standalone instance, block 128) and compare every sample's bits with the native render.
//
//   node tests/wasm_render.mjs --make-input in.wav
//   node tests/wasm_render.mjs <fluent_audio.wasm> <pack.yaml> <look> <in.wav> <native_out.wav> [--bench]
import fs from "node:fs";

function readWav(path) {
  const b = fs.readFileSync(path);
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  let pos = 12, ch = 1, sr = 48000, bits = 32, fmt = 3, data = 0, len = 0;
  while (pos + 8 <= b.length) {
    const id = b.toString("ascii", pos, pos + 4), sz = dv.getUint32(pos + 4, true);
    if (id === "fmt ") { fmt = dv.getUint16(pos + 8, true); ch = dv.getUint16(pos + 10, true); sr = dv.getUint32(pos + 12, true); bits = dv.getUint16(pos + 22, true); }
    if (id === "data") { data = pos + 8; len = sz; break; }
    pos += 8 + sz + (sz & 1);
  }
  if (fmt !== 3 || bits !== 32) throw new Error(path + ": expected float32 WAV");
  const frames = len / 4 / ch, out = [];
  for (let c = 0; c < ch; c++) out.push(new Float32Array(frames));
  for (let i = 0; i < frames; i++) for (let c = 0; c < ch; c++) out[c][i] = dv.getFloat32(data + (i * ch + c) * 4, true);
  return { sr, ch: out };
}

function writeMonoWav(path, x, sr) {
  const b = Buffer.alloc(44 + x.length * 4);
  b.write("RIFF", 0); b.writeUInt32LE(36 + x.length * 4, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(3, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 4, 28);
  b.writeUInt16LE(4, 32); b.writeUInt16LE(32, 34); b.write("data", 36); b.writeUInt32LE(x.length * 4, 40);
  for (let i = 0; i < x.length; i++) b.writeFloatLE(x[i], 44 + i * 4);
  fs.writeFileSync(path, b);
}

if (process.argv[2] === "--make-input") {
  // speech-like: syllables of filtered noise + a gliding tone, gaps between them
  const sr = 48000, n = sr * 3, x = new Float32Array(n);
  let s = 12345, lp = 0, ph = 0;
  for (let i = 0; i < n; i++) {
    s ^= s << 13; s ^= s >>> 17; s ^= s << 5; s >>>= 0;
    const w = (s / 4294967296) * 2 - 1;
    lp = 0.85 * lp + 0.15 * w;
    const syl = Math.floor(i / 9600), inSyl = (i % 9600) < 7200 && syl % 4 !== 3;
    ph += (140 + 40 * Math.sin(i / 4000)) / sr;
    const env = inSyl ? Math.sin(Math.PI * (i % 9600) / 7200) : 0;
    x[i] = env * (0.25 * Math.sin(2 * Math.PI * ph) + 0.3 * lp + 0.05 * w);
  }
  writeMonoWav(process.argv[3], x, sr);
  process.exit(0);
}

const [wasmPath, packPath, look, inPath, nativePath] = process.argv.slice(2);
const bench = process.argv.includes("--bench");
const mod = new WebAssembly.Module(fs.readFileSync(wasmPath));
const imports = {};
for (const im of WebAssembly.Module.imports(mod)) (imports[im.module] ??= {})[im.name] = () => 52;
const ex = new WebAssembly.Instance(mod, imports).exports;
ex._initialize?.();
const cstr = (s) => {
  const u = Buffer.from(s + "\0", "utf8"), p = ex.malloc(u.length);
  new Uint8Array(ex.memory.buffer).set(u, p);
  return p;
};
const readStr = (p) => { const u = new Uint8Array(ex.memory.buffer); let e = p; while (u[e]) e++; return Buffer.from(u.subarray(p, e)).toString("utf8"); };

const input = readWav(inPath);
const sr = input.sr, block = 128;
const yp = cstr(fs.readFileSync(packPath, "utf8"));
const fa = ex.fa_create(yp, sr, block);
if (!fa) { console.error("fa_create:", readStr(ex.fa_error())); process.exit(1); }
const lookP = cstr(look);
if (!ex.fa_select(fa, lookP)) { console.error("no look", look); process.exit(1); }
const buf = ex.memory.buffer;
const in0 = new Float32Array(buf, ex.fa_in_buffer(fa, 0), block);
const o0 = new Float32Array(buf, ex.fa_out_buffer(fa, 0), block), o1 = new Float32Array(buf, ex.fa_out_buffer(fa, 1), block);
const x = input.ch[0], n = x.length + sr * 1;   // fa_render --tail 1 --no-trim
const L = new Float32Array(n), R = new Float32Array(n);
const times = [];
for (let pos = 0; pos < n; pos += block) {
  const m = Math.min(block, n - pos);
  for (let i = 0; i < m; i++) in0[i] = pos + i < x.length ? x[pos + i] : 0;
  const t0 = performance.now();
  ex.fa_process(fa, m);
  times.push(performance.now() - t0);
  L.set(o0.subarray(0, m), pos);
  R.set(o1.subarray(0, m), pos);
}
if (bench) {
  times.sort((a, b) => a - b);
  const mean = times.reduce((a, b) => a + b, 0) / times.length;
  console.log(`${look.padEnd(13)} wasm(node) mean ${(mean * 1000).toFixed(1)} µs  p99 ${(times[Math.floor(times.length * 0.99)] * 1000).toFixed(1)} µs per 128 frames`);
}
if (nativePath && fs.existsSync(nativePath)) {
  const nat = readWav(nativePath);
  if (nat.ch[0].length !== n) { console.error(`${look}: length ${nat.ch[0].length} vs ${n}`); process.exit(1); }
  const a = new Uint32Array(L.buffer), b = new Uint32Array(nat.ch[0].buffer), c = new Uint32Array(R.buffer), d = new Uint32Array(nat.ch[1].buffer);
  let diff = 0, first = -1;
  for (let i = 0; i < n; i++) if (a[i] !== b[i] || c[i] !== d[i]) { diff++; if (first < 0) first = i; }
  if (diff) { console.error(`${look}: ${diff} of ${n} samples differ (first at ${first}: ${L[first]} vs ${nat.ch[0][first]})`); process.exit(1); }
  console.log(`${look}: native and wasm are bit-identical (${n} frames, digest ${readStr(ex.fa_digest(fa))})`);
}
