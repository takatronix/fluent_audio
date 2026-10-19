// widen — M/S width: M=(L+R)/2, S=(L-R)/2, `width` scales S only (folding to mono just loses the side). Below
// `bass_mono_hz` the side is high-passed away. Optional Haas goes into the side only (the mono sum is unchanged).
// A mono voice has no side: widen alone does not spread it — chorus / reverb make the side.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Widen final : public Node {
public:
    enum { kWidth, kBassMono, kHaas };
    static constexpr ParamSpec kParams[] = {
        {"width", "", 0, 2, 1, Smooth::kLinear, "side gain (1 = unchanged)"},
        {"bass_mono_hz", "Hz", 20, 500, 150, Smooth::kLog, "side is high-passed below this"},
        {"haas_ms", "ms", 0, 20, 0, Smooth::kFixed, "side-only Haas delay (0 = off)"},
    };
    static constexpr int kCost = 6;
    static int outChannels(int) { return 2; }

    void prepare(Prepare& pr) override {
        haas_ = static_cast<uint32_t>(p(kHaas) * pr.sample_rate * 0.001 + 0.5);
        if (haas_ > 0) {
            const uint32_t n = dsp::DelayLine::sizeFor(haas_);
            line_.attach(pr.arena.alloc<float>(n), n);
        }
    }
    void reset() noexcept override { hp_st_.reset(); if (haas_) line_.reset(); w_.reset(); }
    void control() noexcept override {
        if (changed(kBassMono)) hp_ = dsp::rbj(dsp::BiquadType::kHp, p(kBassMono), 0.707, 0, sampleRate());
        w_.next(p(kWidth));
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            const float L = l[i], R = s.channels == 2 ? r[i] : l[i];
            const float m = 0.5f * (L + R);
            float sd = hp_st_.run(hp_, 0.5f * (L - R)) * w_.at(s.pos + i);
            if (haas_) {
                line_.push(m);
                sd += 0.3f * (line_.tap(haas_ + 1) - m);
            }
            l[i] = m + sd;
            r[i] = m - sd;
        }
    }

private:
    dsp::BiquadCoef hp_;
    dsp::BiquadState hp_st_;
    dsp::DelayLine line_;
    uint32_t haas_ = 0;
    dsp::Ramp w_;
};

FA_NODE(widen, Widen, "Mono-safe M/S width with bass-mono and optional side-only Haas.")

}  // namespace fluent_audio
