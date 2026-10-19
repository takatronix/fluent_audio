// exciter — "air": only the band above `freq` is driven through tanh (ADAA), high-passed again so nothing muddy
// is added, and a little of it is mixed in. Works on voices that have nothing above 8 kHz to boost.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Exciter final : public Node {
public:
    enum { kFreq, kDrive, kMix };
    static constexpr ParamSpec kParams[] = {
        {"freq", "Hz", 1000, 12000, 4500, Smooth::kLog, "only this band and above is excited"},
        {"drive", "x", 1, 10, 2, Smooth::kLinear, "drive into the tanh"},
        {"mix", "", 0, 0.5f, 0.08f, Smooth::kLinear, "amount of harmonics added"},
    };
    static constexpr int kCost = 25;

    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) { a_[c].reset(); b_[c].reset(); sat_[c].reset(); }
        mix_.reset();
    }
    void control() noexcept override {
        if (changed(kFreq)) hp_ = dsp::rbj(dsp::BiquadType::kHp, p(kFreq), 0.707, 0, sampleRate());
        drive_ = p(kDrive);
        inv_drive_ = 1.0f / drive_;
        mix_.next(p(kMix));
    }
    void process(Slice& s) noexcept override {
        for (int c = 0; c < s.channels; ++c) {
            float* x = s.ch[c];
            for (int i = 0; i < s.n; ++i) {
                const float h = sat_[c](drive_ * a_[c].run(hp_, x[i])) * inv_drive_;
                x[i] += mix_.at(s.pos + i) * b_[c].run(hp_, h);
            }
        }
    }

private:
    dsp::BiquadCoef hp_;
    dsp::BiquadState a_[2], b_[2];
    dsp::AdaaTanh sat_[2];
    float drive_ = 2, inv_drive_ = 0.5f;
    dsp::Ramp mix_;
};

FA_NODE(exciter, Exciter, "High-band harmonic exciter (tanh with ADAA on the band above `freq`).")

}  // namespace fluent_audio
