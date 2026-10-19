// fluent_audio — AudioWorkletProcessor (design §3.2). Runs fluent_audio.wasm directly in the audio thread:
// no SharedArrayBuffer, no Emscripten glue, no fetch / TextDecoder here (some browsers lack them in worklets).
//
// processorOptions: { module: WebAssembly.Module (compiled on the main thread) | bytes: ArrayBuffer,
//                     yaml: Uint8Array (UTF-8, NUL not needed), maxBlock?: number,
//                     preset?: Uint8Array (UTF-8 name: the look to start with, no crossfade) }
// port in:  {select: idx} {param: idx, value} {bypass: 0|1} {reset: true}
// port out: {type: "ready", presets, aliases, params, digest, latency, memory} | {type: "error", message}
//           {type: "a", a: [frame, voice, level, peak, onset, centroid, tail]}  ~60 Hz
// The whole look bank is built here once, before the page connects the node (no YAML parsing later).

const ANALYSIS_EVERY = 6;   // blocks of 128 at 48 kHz ≈ 60 Hz

class FluentAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.ok = false;
    this.count = 0;
    this.port.onmessage = (ev) => this.onMessage(ev.data);
    try {
      let mod = o.module;
      if (!(mod instanceof WebAssembly.Module)) mod = new WebAssembly.Module(o.bytes);   // sync compile, once
      const stub = () => 52;   // ENOSYS: the core never does I/O; these only exist for libc's stderr
      const imports = {};
      for (const im of WebAssembly.Module.imports(mod)) {
        (imports[im.module] = imports[im.module] || {})[im.name] = stub;
      }
      const ex = this.ex = new WebAssembly.Instance(mod, imports).exports;
      if (ex._initialize) ex._initialize();
      this.maxBlock = o.maxBlock || 1024;
      const y = o.yaml;
      const p = ex.malloc(y.length + 1);
      const u8 = new Uint8Array(ex.memory.buffer);
      u8.set(y, p);
      u8[p + y.length] = 0;
      this.fa = ex.fa_create(p, sampleRate, this.maxBlock);
      ex.free(p);
      if (!this.fa) throw new Error("fa_create: " + this.str(ex.fa_error()));
      if (o.preset && o.preset.length) {   // before the first sample: instant
        const q = ex.malloc(o.preset.length + 1);
        const m8 = new Uint8Array(ex.memory.buffer);
        m8.set(o.preset, q);
        m8[q + o.preset.length] = 0;
        ex.fa_select(this.fa, q);
        ex.free(q);
      }
      // memory never grows (16 MB fixed), so these views stay valid
      const buf = ex.memory.buffer;
      this.in0 = new Float32Array(buf, ex.fa_in_buffer(this.fa, 0), this.maxBlock);
      this.out0 = new Float32Array(buf, ex.fa_out_buffer(this.fa, 0), this.maxBlock);
      this.out1 = new Float32Array(buf, ex.fa_out_buffer(this.fa, 1), this.maxBlock);
      const ap = ex.fa_analysis(this.fa);
      this.aU32 = new Uint32Array(buf, ap, 4);
      this.aF32 = new Float32Array(buf, ap + 16, 6);
      const presets = [], aliases = {}, params = [];
      for (let i = 0; i < ex.fa_preset_count(this.fa); i++) presets.push(this.str(ex.fa_preset_name(this.fa, i)));
      for (let i = 0; i < ex.fa_alias_count(this.fa); i++) aliases[this.str(ex.fa_alias_name(this.fa, i))] = ex.fa_alias_preset(this.fa, i);
      for (let i = 0; i < ex.fa_param_count(this.fa); i++) params.push(this.str(ex.fa_param_name(this.fa, i)));
      this.ok = true;
      this.port.postMessage({ type: "ready", presets, aliases, params, digest: this.str(ex.fa_digest(this.fa)),
                              latency: ex.fa_latency_samples(this.fa), memory: ex.fa_memory_bytes(this.fa), sampleRate });
    } catch (e) {
      this.port.postMessage({ type: "error", message: String((e && e.message) || e) });
    }
  }

  // UTF-8 C string → JS string, without TextDecoder
  str(ptr) {
    if (!ptr) return "";
    const u8 = new Uint8Array(this.ex.memory.buffer);
    let s = "", i = ptr;
    while (u8[i]) {
      let c = u8[i++];
      if (c >= 0xf0) c = ((c & 7) << 18) | ((u8[i++] & 63) << 12) | ((u8[i++] & 63) << 6) | (u8[i++] & 63);
      else if (c >= 0xe0) c = ((c & 15) << 12) | ((u8[i++] & 63) << 6) | (u8[i++] & 63);
      else if (c >= 0xc0) c = ((c & 31) << 6) | (u8[i++] & 63);
      s += String.fromCodePoint(c);
    }
    return s;
  }

  onMessage(m) {
    if (!this.ok || !m) return;
    const ex = this.ex, fa = this.fa;
    if (m.select !== undefined) ex.fa_select_idx(fa, m.select | 0);
    if (m.param !== undefined) ex.fa_set_param_idx(fa, m.param | 0, +m.value);
    if (m.bypass !== undefined) ex.fa_bypass(fa, m.bypass ? 1 : 0);
    if (m.reset) ex.fa_reset(fa);
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || !out.length) return true;
    const n = out[0].length;
    const inp = inputs[0];
    if (!this.ok) {   // failed: pass the input through (the page should have switched to its dry path anyway)
      for (let c = 0; c < out.length; c++) {
        if (inp && inp.length) out[c].set(inp[Math.min(c, inp.length - 1)]);
        else out[c].fill(0);
      }
      return true;
    }
    try {
      for (let off = 0; off < n; off += this.maxBlock) {
        const m = Math.min(this.maxBlock, n - off);
        if (!inp || !inp.length) this.in0.fill(0, 0, m);
        else if (inp.length === 1) this.in0.set(inp[0].subarray(off, off + m));
        else {
          const a = inp[0], b = inp[1];
          for (let i = 0; i < m; i++) this.in0[i] = 0.5 * (a[off + i] + b[off + i]);
        }
        this.ex.fa_process(this.fa, m);
        out[0].set(this.out0.subarray(0, m), off);
        if (out.length > 1) out[1].set(this.out1.subarray(0, m), off);
      }
      if (++this.count >= ANALYSIS_EVERY) {
        this.count = 0;
        const f = this.aF32, u = this.aU32;
        this.port.postMessage({ type: "a", a: [u[2] + u[3] * 4294967296, f[0], f[1], f[2], f[3], f[4], f[5]] });
      }
    } catch (e) {
      this.ok = false;
      this.port.postMessage({ type: "error", message: "process: " + String((e && e.message) || e) });
    }
    return true;   // keep running through silences and reverb tails
  }
}

registerProcessor("fluent-audio", FluentAudioProcessor);
