// saturate — gentle tanh drive, level-compensated (tanh(drive·x)/tanh(drive)), first-order ADAA against aliasing
// (half a sample of delay on the wet). `asym` adds even harmonics; a DC blocker then follows.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Saturate final : public Node {
public:
    enum { kDrive, kMix, kAsym };
    static constexpr ParamSpec kParams[] = {
        {"drive", "x", 1, 10, 1.5f, Smooth::kLinear, "drive into the curve"},
        {"mix", "", 0, 1, 1, Smooth::kLinear, "wet amount"},
        {"asym", "", 0, 0.5f, 0, Smooth::kFixed, "bias for even harmonics (adds a DC blocker)"},
    };
    static constexpr int kCost = 15;

    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) { sat_[c].reset(); dc_x_[c] = dc_y_[c] = 0; }
        mix_.reset();
    }
    void control() noexcept override {
        drive_ = p(kDrive);
        asym_ = p(kAsym);
        bias_ = dsp::ftanh(asym_);
        norm_ = 1.0f / dsp::ftanh(drive_);
        mix_.next(p(kMix));
        dc_a_ = static_cast<float>(dsp::poleHz(10.0, sampleRate()));
    }
    void process(Slice& s) noexcept override {
        for (int c = 0; c < s.channels; ++c) {
            float* x = s.ch[c];
            for (int i = 0; i < s.n; ++i) {
                float y = (sat_[c](drive_ * x[i] + asym_) - bias_) * norm_;
                if (asym_ > 0.0f) {   // one-pole DC blocker
                    const float o = y - dc_x_[c] + dc_a_ * dc_y_[c];
                    dc_x_[c] = y;
                    dc_y_[c] = dsp::flush(o);
                    y = o;
                }
                const float m = mix_.at(s.pos + i);
                x[i] = x[i] * (1.0f - m) + y * m;
            }
        }
    }

private:
    dsp::AdaaTanh sat_[2];
    float drive_ = 1.5f, asym_ = 0, bias_ = 0, norm_ = 1, dc_a_ = 0;
    float dc_x_[2] = {0, 0}, dc_y_[2] = {0, 0};
    dsp::Ramp mix_;
};

FA_NODE(saturate, Saturate, "Gentle tanh saturation, level-compensated, ADAA anti-aliasing.")

}  // namespace fluent_audio
