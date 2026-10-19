# fluent_audio

**Declarative, real-time audio effects for voices — one C++ core, native and in the browser, same bits.**

Status: **M1** (2026-10-19). Kepler's voice gets a per-look sound (kepler, fluent glass/metal, gargantua, kitt,
hal, luna/clean; system and comic alias kepler) with 0 samples of added latency.

```
Sound YAML (chain + buses + master, $params with animate, macros) → Engine (C++17, block ≤ 32 per control tick)
   → native: libfluent_audio.so, fa_render, Python (ctypes)
   → wasm in an AudioWorklet (230 KB, no SharedArrayBuffer) — bit-identical to native
```

```sh
export PATH=~/.local/share/fs-toolchain/bin:$PATH          # g++ / cmake / ninja on farm3
cmake -S . -B build -G Ninja && cmake --build build && (cd build && ctest)
build/fa_render presets/looks.yaml fluent voice.wav out.wav    # offline render of one look
build/fa_render --list                                         # every node and its params
source ~/emsdk/emsdk_env.sh && ./wasm/build.sh                 # → wasm/dist/
```

- C API: [include/fluent_audio/fa.h](include/fluent_audio/fa.h) (`fa_create`, `fa_process`, `fa_select`, `fa_set_param_f32` …)
- Browser: `FluentAudio.create(ctx, yaml)` → `.select(look)`, `.set(name, v)`, `.analysis` ([wasm/fluent-audio.js](wasm/fluent-audio.js))
- Add a filter: [docs/howto-add-filter.ja.md](docs/howto-add-filter.ja.md) — one file per node (`FA_NODE`), or a YAML macro
- Nodes: [docs/nodes.md](docs/nodes.md) (generated) · Design (Japanese): [docs/design/fluent_audio.ja.md](docs/design/fluent_audio.ja.md)
- Sibling: [fluent_scene](https://github.com/takatronix/fluent_scene) — the same idea for pictures

Not to be confused with the ROS 2 package of the same name inside fluent_vision_ros2 (`fv_audio*`):
this library has no ROS dependency; a ROS 2 node can wrap it later.

License: MIT · by @takatronix
