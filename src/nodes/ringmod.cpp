// ringmod — a faint low-frequency ring modulation (1 - mix + mix·sin): an electronic grain, not a robot voice.
// Range kept low on purpose (design §8: gimmicks are capped).
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Ringmod final : public Node {
public:
    enum { kFreq, kMix };
    static constexpr ParamSpec kParams[] = {
        {"freq", "Hz", 1, 200, 50, Smooth::kLog, "carrier frequency"},
        {"mix", "", 0, 0.15f, 0.05f, Smooth::kLinear, "modulation depth"},
    };
    static constexpr int kCost = 4;

    void reset() noexcept override { phase_ = 0; mix_.reset(); }
    void control() noexcept override {
        inc_ = static_cast<float>(p(kFreq) / sampleRate());
        mix_.next(p(kMix));
    }
    void process(Slice& s) noexcept override {
        for (int i = 0; i < s.n; ++i) {
            const float m = mix_.at(s.pos + i);
            const float g = 1.0f - m + m * dsp::sinTurns(phase_);
            phase_ += inc_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            for (int c = 0; c < s.channels; ++c) s.ch[c][i] *= g;
        }
    }

private:
    float phase_ = 0, inc_ = 0;
    dsp::Ramp mix_;
};

FA_NODE(ringmod, Ringmod, "Faint low-frequency ring modulation (electronic grain).")

}  // namespace fluent_audio
