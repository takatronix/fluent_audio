// fluent_audio — page-side wrapper (classic script; defines window.FluentAudio).
//
//   const fx = await FluentAudio.create(ctx, packYamlText);    // or { url: "/vendor/fluent_audio/looks.pack.yaml" }
//   source.connect(fx.node); fx.node.connect(ctx.destination);
//   fx.select("kitt");            // a look name; aliases (system → kepler) resolve
//   fx.set("amount", 0.5);        // any param name (fx.params lists them)
//   fx.analysis                   // { frame, voice, level, peak, onset, centroid, tail }, updated ~60 Hz
//   fx.onerror = e => ...         // the worklet failed: reconnect your dry path
// opts: { base, wasmUrl, workletUrl, timeout (ms), preset (the look to start with, no crossfade) }
//
// The wasm is compiled here on the main thread and handed to the worklet as a WebAssembly.Module (no compile on
// the audio thread); if the browser cannot clone a Module into the worklet, the bytes go instead. create()
// resolves only after the worklet answered {ready} (all looks built) and rejects on {error} or after a timeout.
(function (g) {
  "use strict";
  const here = (() => {
    try { return new URL(".", document.currentScript.src).href; } catch (e) { return "/vendor/fluent_audio/"; }
  })();

  function utf8(s) {
    if (typeof TextEncoder !== "undefined") return new TextEncoder().encode(s);
    const u = unescape(encodeURIComponent(s)), a = new Uint8Array(u.length);
    for (let i = 0; i < u.length; i++) a[i] = u.charCodeAt(i);
    return a;
  }

  class Instance {
    constructor(node, ready) {
      this.node = node;
      this.presets = ready.presets;
      this.aliases = ready.aliases;
      this.params = ready.params;
      this.digest = ready.digest;
      this.latency = ready.latency;
      this.memory = ready.memory;
      this.failed = false;
      this.analysis = { frame: 0, voice: 0, level: 0, peak: 0, onset: 0, centroid: 0, tail: 0 };
      this.analysisTime = 0;
      this.onanalysis = null;
      this.onerror = null;
      this._paramIdx = new Map(this.params.map((n, i) => [n, i]));
      this._pending = new Map();
      this._current = -1;
      node.port.onmessage = (ev) => {
        const m = ev.data;
        if (m.type === "a") {
          const a = m.a, an = this.analysis;
          an.frame = a[0]; an.voice = a[1]; an.level = a[2]; an.peak = a[3]; an.onset = a[4]; an.centroid = a[5]; an.tail = a[6];
          this.analysisTime = performance.now();
          if (this.onanalysis) this.onanalysis(an);
        } else if (m.type === "error") this._fail(m.message);
      };
      node.onprocessorerror = () => this._fail("processorerror");
    }
    _fail(msg) {
      if (this.failed) return;
      this.failed = true;
      if (this.onerror) this.onerror(new Error(msg));
    }
    presetIndex(name) {
      const i = this.presets.indexOf(name);
      if (i >= 0) return i;
      return name in this.aliases ? this.aliases[name] : -1;
    }
    select(name) {
      const i = typeof name === "number" ? name : this.presetIndex(name);
      if (i < 0 || i === this._current) return i >= 0;
      this._current = i;
      this.node.port.postMessage({ select: i });
      return true;
    }
    // Continuous values are coalesced: the latest value per param, sent once per microtask.
    set(name, value) {
      const i = this._paramIdx.get(name);
      if (i === undefined) return false;
      const first = this._pending.size === 0;
      this._pending.set(i, +value);
      if (first) queueMicrotask(() => {
        for (const [param, v] of this._pending) this.node.port.postMessage({ param, value: v });
        this._pending.clear();
      });
      return true;
    }
    bypass(on) { this.node.port.postMessage({ bypass: on ? 1 : 0 }); }
    reset() { this.node.port.postMessage({ reset: true }); }
    destroy() { try { this.node.disconnect(); } catch (e) {} this.node.port.onmessage = null; }
  }

  async function create(ctx, yaml, opts) {
    opts = opts || {};
    if (!ctx || !ctx.audioWorklet || typeof AudioWorkletNode === "undefined") throw new Error("AudioWorklet unavailable");
    const base = opts.base || here;
    if (yaml && typeof yaml === "object" && yaml.url) {
      const r = await fetch(yaml.url, { cache: "no-cache" });
      if (!r.ok) throw new Error("yaml " + r.status);
      yaml = await r.text();
    }
    const [bytes] = await Promise.all([
      fetch(opts.wasmUrl || base + "fluent_audio.wasm", { cache: "no-cache" }).then(r => {
        if (!r.ok) throw new Error("wasm " + r.status);
        return r.arrayBuffer();
      }),
      ctx.audioWorklet.addModule(opts.workletUrl || base + "fa-worklet.js"),
    ]);
    const module = await WebAssembly.compile(bytes);   // not compileStreaming: no MIME type dependency
    const y = typeof yaml === "string" ? utf8(yaml) : yaml;
    const nodeOpts = (po) => ({
      numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
      channelCount: 1, channelCountMode: "explicit", channelInterpretation: "speakers",   // the voice: mono in
      processorOptions: po,
    });
    const preset = opts.preset ? utf8(opts.preset) : null;
    let node;
    try {   // the Module (no compile on the audio thread) and the bytes in case the Module arrives unusable
      node = new AudioWorkletNode(ctx, "fluent-audio", nodeOpts({ module, bytes, yaml: y, maxBlock: 1024, preset }));
    } catch (e) {   // the Module could not be cloned into the worklet: the bytes only
      node = new AudioWorkletNode(ctx, "fluent-audio", nodeOpts({ bytes, yaml: y, maxBlock: 1024, preset }));
    }
    const ready = await new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error("worklet timeout")), opts.timeout || 5000);
      node.port.onmessage = (ev) => {
        if (ev.data.type === "ready") { clearTimeout(t); resolve(ev.data); }
        else if (ev.data.type === "error") { clearTimeout(t); reject(new Error(ev.data.message)); }
      };
      node.onprocessorerror = () => { clearTimeout(t); reject(new Error("processorerror")); };
    });
    const inst = new Instance(node, ready);
    if (opts.preset) inst._current = inst.presetIndex(opts.preset);
    return inst;
  }

  g.FluentAudio = { create, version: "0.1.0" };
})(typeof window !== "undefined" ? window : globalThis);
