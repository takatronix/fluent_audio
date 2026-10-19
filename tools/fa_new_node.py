#!/usr/bin/env python3
"""Scaffold a new fluent_audio node: one C++ file + a golden test case.

  python3 tools/fa_new_node.py <name> [--stereo]

Writes src/nodes/<name>.cpp (a working template: a smoothed gain with a sine wobble — replace the DSP) and
tests/cases/<name>.yaml (a Sound using the node, rendered by the golden test). Nothing else needs editing: the
build globs src/nodes/, FA_NODE registers the node, and the YAML loader, C API, wasm/JS wrapper, Python and
`fa_render --list --markdown` all read its parameter table. Then:

  cmake --build build && build/fa_tests nodes        # the generic tests every node gets
  build/fa_tests golden --update                     # writes tests/golden/node_<name>.wav + case_<name>.wav
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEMPLATE = '''// {name} — TODO: one line on what it does and how (the algorithm, its latency).
#include "fluent_audio/node.hpp"

namespace fluent_audio {{

class {cls} final : public Node {{
public:
    enum {{ kAmount, kRate }};
    static constexpr ParamSpec kParams[] = {{
        // name      unit  min  max   default smoothing        help
        {{"amount", "",   0,   1,    0.2f,   Smooth::kLinear, "how much"}},
        {{"rate_hz", "Hz", 0.1f, 20, 4,     Smooth::kLog,    "wobble speed"}},
    }};
    static constexpr int kCost = 6;                                   // ~ops per sample (budget check)
{stereo}
    void prepare(Prepare& p) override {{}}                             // allocate here only: p.arena.alloc<float>(n)
    void reset() noexcept override {{ phase_ = 0; amt_.reset(); }}
    void control() noexcept override {{                                // every 32 samples, after params moved
        inc_ = static_cast<float>(p(kRate) / sampleRate());
        amt_.next(p(kAmount));
    }}
    void process(Slice& s) noexcept override {{                        // n <= 32 samples; no alloc / locks / I/O
        for (int i = 0; i < s.n; ++i) {{
            const float g = 1.0f - amt_.at(s.pos + i) * (0.5f + 0.5f * dsp::sinTurns(phase_));
            phase_ += inc_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            for (int c = 0; c < s.channels; ++c) s.ch[c][i] *= g;
        }}
    }}

private:
    float phase_ = 0, inc_ = 0;
    dsp::Ramp amt_;
}};

FA_NODE({name}, {cls}, "TODO: one-line help shown in docs and UIs.")

}}  // namespace fluent_audio
'''

STEREO = '''    static int outChannels(int) { return 2; }                         // mono in → stereo out
'''

CASE = '''# Golden case for the `{name}` node (tests/test_golden.cpp renders it; `fa_tests golden --update` writes the WAV).
schema: fluent.sound/v1alpha1
name: case_{name}
chain:
  - {name}: {{ amount: 0.3 }}
'''


def main():
    if len(sys.argv) < 2 or not re.fullmatch(r"[a-z][a-z0-9_]*", sys.argv[1]):
        sys.exit(__doc__)
    name = sys.argv[1]
    cls = "".join(w.capitalize() for w in name.split("_")) + "Node"
    src = os.path.join(ROOT, "src", "nodes", name + ".cpp")
    case = os.path.join(ROOT, "tests", "cases", name + ".yaml")
    for f in (src, case):
        if os.path.exists(f):
            sys.exit(f"{f} exists")
    if os.path.exists(os.path.join(ROOT, "macros", name + ".yaml")):
        sys.exit(f"macros/{name}.yaml already uses this name")
    with open(src, "w") as f:
        f.write(TEMPLATE.format(name=name, cls=cls, stereo=STEREO if "--stereo" in sys.argv else ""))
    os.makedirs(os.path.dirname(case), exist_ok=True)
    with open(case, "w") as f:
        f.write(CASE.format(name=name))
    print(f"wrote {os.path.relpath(src, ROOT)} and {os.path.relpath(case, ROOT)}")
    print("next: cmake --build build && build/fa_tests nodes && build/fa_tests golden --update")


if __name__ == "__main__":
    main()
