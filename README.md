# fluent_audio

**Declarative, real-time audio effects for voices and UI sounds — one C++ core, native and in the browser.**

Status: **design stage** (2026-10-19). Nothing is implemented yet except an offline Python sketch
(`sketch/`) that renders the look presets so the direction can be heard.

```
Sound YAML (graph + $params) → Graph (C++ runtime, block 128) → native (miniaudio / files)
                                                              → wasm in an AudioWorklet (browser)
```

- Design (Japanese): [docs/design/fluent_audio.ja.md](docs/design/fluent_audio.ja.md)
- Presets per look (draft): [presets/](presets/)
- Sibling: [fluent_scene](https://github.com/takatronix/fluent_scene) — same idea for pictures

Not to be confused with the ROS 2 package of the same name inside fluent_vision_ros2 (`fv_audio*`):
this library has no ROS dependency; a ROS 2 node can wrap it later.

License: MIT (planned) · by @takatronix
