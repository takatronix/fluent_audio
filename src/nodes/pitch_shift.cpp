// pitch_shift — Doppler-type shifter: two taps sweep a delay window in opposite phase, crossfaded with sin²
// (the gains always sum to 1). Cheap and smeary — meant for wet paths (shimmer), where the reverb hides it.
// Shifting up reads faster than real time, so a 4th-order low-pass at fs/(2·ratio) comes first (no aliasing).
// Output is mono (stereo in is summed).
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class PitchShift final : public Node {
public:
    enum { kSemitones, kWindow };
    static constexpr ParamSpec kParams[] = {
        {"semitones", "st", -24, 24, 12, Smooth::kLinear, "shift (12 = an octave up)"},
        {"window_ms", "ms", 10, 100, 50, Smooth::kFixed, "sweep window (longer = smoother, more smear)"},
    };
    static constexpr int kCost = 30;
    static int outChannels(int) { return 1; }

    void prepare(Prepare& pr) override {
        w_ = static_cast<float>(p(kWindow) * pr.sample_rate * 0.001);
        const uint32_t n = dsp::DelayLine::sizeFor(static_cast<uint32_t>(w_) + 8);
        line_.attach(pr.arena.alloc<float>(n), n);
    }
    void reset() noexcept override {
        line_.reset();
        lp_[0].reset();
        lp_[1].reset();
        ph_ = 0;
    }
    void control() noexcept override {
        if (!changed()) return;
        const double ratio = dsp::dexp(p(kSemitones) / 12.0 * dsp::kLn2);
        dph_ = static_cast<float>((1.0 - ratio) / w_);
        double fc = 0.45 * sampleRate();
        if (ratio > 1.0 && 0.9 * sampleRate() / (2.0 * ratio) < fc) fc = 0.9 * sampleRate() / (2.0 * ratio);
        lc_[0] = dsp::rbj(dsp::BiquadType::kLp, fc, 0.5412, 0, sampleRate());   // Butterworth 4th order
        lc_[1] = dsp::rbj(dsp::BiquadType::kLp, fc, 1.3066, 0, sampleRate());
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        for (int i = 0; i < s.n; ++i) {
            float x = s.channels == 2 ? 0.5f * (l[i] + s.ch[1][i]) : l[i];
            x = lp_[1].run(lc_[1], lp_[0].run(lc_[0], x));
            line_.push(x);
            ph_ += dph_;
            ph_ -= dsp::ffloor(ph_);
            float ph2 = ph_ + 0.5f;
            if (ph2 >= 1.0f) ph2 -= 1.0f;
            const float s1 = dsp::sinTurns(0.5f * ph_), s2 = dsp::sinTurns(0.5f * ph2);
            l[i] = line_.hermite(2.0f + ph_ * w_) * (s1 * s1) + line_.hermite(2.0f + ph2 * w_) * (s2 * s2);
        }
    }

private:
    dsp::DelayLine line_;
    dsp::BiquadCoef lc_[2];
    dsp::BiquadState lp_[2];
    float w_ = 2400, ph_ = 0, dph_ = 0;
};

FA_NODE(pitch_shift, PitchShift, "Doppler pitch shifter for wet paths (with anti-alias low-pass when going up).")

}  // namespace fluent_audio
