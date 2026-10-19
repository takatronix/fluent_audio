// phaser — slow sweeping notches, "light through water": 2..8 first-order allpasses whose corner glides between
// `low_hz` and `high_hz` (log-sine LFO), with a little feedback, summed with the dry. The right channel's LFO runs
// `stereo` turns ahead, so the notches move differently in each ear (motion and width). out = (1-d/2)·x + d/2·ap:
// depth 1 gives full notches and never boosts. Mono in → stereo out; zero latency.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Phaser final : public Node {
public:
    enum { kStages, kRate, kLow, kHigh, kFeedback, kDepth, kStereo };
    static constexpr ParamSpec kParams[] = {
        {"stages", "", 2, 8, 4, Smooth::kFixed, "allpass stages (notches = stages / 2)"},
        {"rate_hz", "Hz", 0.01f, 5, 0.15f, Smooth::kLog, "sweep speed"},
        {"low_hz", "Hz", 50, 4000, 300, Smooth::kLog, "bottom of the sweep"},
        {"high_hz", "Hz", 200, 16000, 3000, Smooth::kLog, "top of the sweep"},
        {"feedback", "", 0, 0.9f, 0.2f, Smooth::kLinear, "resonance of the notches"},
        {"depth", "", 0, 1, 0.5f, Smooth::kLinear, "0 = dry, 1 = full notches"},
        {"stereo", "turns", 0, 0.5f, 0.25f, Smooth::kLinear, "right LFO phase offset"},
    };
    static constexpr int kCost = 40;
    static int outChannels(int) { return 2; }
    static constexpr int kMaxStages = 8;

    void prepare(Prepare&) override { stages_ = static_cast<int>(p(kStages)); }
    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < kMaxStages; ++k) xs_[c][k] = ys_[c][k] = 0;
            fb_[c] = 0;
            a_prev_[c] = a_[c] = -2;
        }
        phase_ = 0;
        depth_.reset();
    }
    void control() noexcept override {
        const double sr = sampleRate();
        const double lo = dsp::dlog(p(kLow)), hi = dsp::dlog(p(kHigh) > p(kLow) ? p(kHigh) : p(kLow));
        for (int c = 0; c < 2; ++c) {
            float t = phase_ + (c ? p(kStereo) : 0.0f);
            if (t >= 1.0f) t -= 1.0f;
            const double f = dsp::dexp(lo + (hi - lo) * (0.5 + 0.5 * dsp::sinTurns(t)));
            double sw, cw;
            dsp::dsincos(dsp::kPi * f / sr, sw, cw);   // tan(πf/fs) = sw / cw
            const float a = static_cast<float>((sw - cw) / (sw + cw));
            a_prev_[c] = a_[c] < -1.5f ? a : a_[c];
            a_[c] = a;
        }
        phase_ += static_cast<float>(p(kRate) * kControlRate / sr);
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        fbk_ = p(kFeedback);
        depth_.next(p(kDepth));
    }
    void process(Slice& s) noexcept override {
        const bool st = s.channels == 2;
        for (int i = 0; i < s.n; ++i) {
            const float k = static_cast<float>(s.pos + i + 1) * (1.0f / kControlRate);
            const float d = 0.5f * depth_.at(s.pos + i);
            const float x0 = s.ch[0][i], x1 = st ? s.ch[1][i] : x0;
            for (int c = 0; c < 2; ++c) {
                const float x = c ? x1 : x0;
                const float a = a_prev_[c] + (a_[c] - a_prev_[c]) * k;
                float u = x + fbk_ * fb_[c];
                for (int j = 0; j < stages_; ++j) {   // first-order allpass: y = a·u + x1 − a·y1
                    const float y = dsp::flush(a * u + xs_[c][j] - a * ys_[c][j]);
                    xs_[c][j] = u;
                    ys_[c][j] = y;
                    u = y;
                }
                fb_[c] = u;
                s.ch[c][i] = (1.0f - d) * x + d * u;
            }
        }
    }

private:
    int stages_ = 4;
    float xs_[2][kMaxStages] = {}, ys_[2][kMaxStages] = {}, fb_[2] = {}, a_[2] = {-2, -2}, a_prev_[2] = {-2, -2};
    float phase_ = 0, fbk_ = 0;
    dsp::Ramp depth_;
};

FA_NODE(phaser, Phaser, "Slow stereo phaser (2-8 allpass stages, log sweep, feedback), mono in → stereo out.")

}  // namespace fluent_audio
