#!/usr/bin/env bash
# native ↔ wasm: the same input through the same looks must give the same bits (design §9).
# Skips (77) when node or wasm/dist/fluent_audio.wasm is missing.
set -euo pipefail
FA_RENDER=$1; SRC=$2
WASM=$SRC/wasm/dist/fluent_audio.wasm
command -v node >/dev/null || exit 77
[[ -f $WASM ]] || { echo "no $WASM (run wasm/build.sh)"; exit 77; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
node "$SRC/tests/wasm_render.mjs" --make-input "$T/in.wav"
for look in kepler luna fluent fluent_metal fluent_silk fluent_water fluent_halo fluent_velvet fluent_space fluent_glass kitt hal gargantua; do
  "$FA_RENDER" "$SRC/presets/looks.yaml" "$look" "$T/in.wav" "$T/native_$look.wav" --no-trim --tail 1
  node "$SRC/tests/wasm_render.mjs" "$WASM" "$SRC/wasm/dist/looks.pack.yaml" "$look" "$T/in.wav" "$T/native_$look.wav"
done
