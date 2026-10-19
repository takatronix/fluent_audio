// limiter — a safety limiter, not a transparent true-peak one (design §4): a fast linked peak compressor and,
// last, a bounded soft-clip curve, so the output can never exceed the ceiling. No look-ahead in v1 (0 latency).
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Limiter final : public Node {
public:
    enum { kCeiling, kRelease, kLookahead };
    static constexpr ParamSpec kParams[] = {
        {"ceiling_db", "dB", -24, 0, -1, Smooth::kLinear, "output never exceeds this sample peak"},
        {"release_ms", "ms", 5, 1000, 80, Smooth::kLinear, "gain recovery"},
        {"lookahead_ms", "ms", 0, 0, 0, Smooth::kFixed, "look-ahead (v2; must be 0 in v1)"},
    };
    static constexpr int kCost = 12;

    void reset() noexcept override { g_ = 1; }
    void control() noexcept override {
        if (!changed()) return;
        c_ = static_cast<float>(dsp::db2lin(p(kCeiling)));
        inv_c_ = 1.0f / c_;
        aa_ = static_cast<float>(dsp::poleMs(0.3, sampleRate()));
        ar_ = static_cast<float>(dsp::poleMs(p(kRelease), sampleRate()));
    }
    static float softclip(float u) {   // |result| < 1 for any input; identity below 0.85
        const float a = u < 0 ? -u : u;
        if (a <= 0.85f) return u;
        const float y = 0.85f + 0.15f * dsp::ftanh((a - 0.85f) * (1.0f / 0.15f));
        return u < 0 ? -y : y;
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            float pk = l[i] < 0 ? -l[i] : l[i];
            if (s.channels == 2) { const float b = r[i] < 0 ? -r[i] : r[i]; if (b > pk) pk = b; }
            const float want = pk > c_ ? c_ / pk : 1.0f;
            g_ = want + (want < g_ ? aa_ : ar_) * (g_ - want);
            l[i] = c_ * softclip(l[i] * g_ * inv_c_);
            if (s.channels == 2) r[i] = c_ * softclip(r[i] * g_ * inv_c_);
        }
    }

private:
    float g_ = 1, c_ = 1, inv_c_ = 1, aa_ = 0, ar_ = 0;
};

FA_NODE(limiter, Limiter, "Safety limiter: linked peak compressor + bounded soft clip. 0 latency.")

}  // namespace fluent_audio
