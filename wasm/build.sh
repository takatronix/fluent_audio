#!/usr/bin/env bash
# Builds fluent_audio for the browser: one standalone .wasm (no JS glue, no SharedArrayBuffer, fixed 16 MB
# memory) that the AudioWorklet instantiates itself (design §3.2), plus the worklet module, the page-side
# wrapper and the packed look bank.
#
#   source ~/emsdk/emsdk_env.sh && ./wasm/build.sh
#
# Out: wasm/dist/{fluent_audio.wasm, fa-worklet.js, fluent-audio.js, looks.pack.yaml}
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p wasm/dist wasm/gen
python3 tools/embed_macros.py wasm/gen/builtin_macros.cpp macros/*.yaml

EXPORTS=$(grep -oE '\bfa_[a-z0-9_]+\(' include/fluent_audio/fa.h | tr -d '(' | sort -u | sed 's/^/_/' | paste -sd, -)
# Same numbers as native (design D8): no FMA contraction, no fast-math. No SIMD in v1 (scalar, bit-exact).
em++ -O2 -std=c++17 -ffp-contract=off -fno-fast-math -fno-exceptions -I include \
  src/core/*.cpp src/sound/*.cpp src/capi.cpp src/nodes/*.cpp wasm/gen/builtin_macros.cpp \
  -sSTANDALONE_WASM --no-entry \
  -sINITIAL_MEMORY=16MB -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=256KB -sFILESYSTEM=0 \
  -sEXPORTED_FUNCTIONS="$EXPORTS,_malloc,_free" \
  -o wasm/dist/fluent_audio.wasm
cp wasm/fa-worklet.js wasm/fluent-audio.js wasm/dist/
python3 tools/fa_pack.py presets/looks.yaml > wasm/dist/looks.pack.yaml
ls -la wasm/dist/
