// microshift — a stereo detune doubler ("micro pitch shift"): two Doppler shifters, one a few cents up (left) and
// one a few cents down (right), each with its own short delay, added to the untouched dry. Smoother than a chorus:
// the pitch offset is constant (no vibrato), so the doubling reads as width and silk, not as wobble. The taps of
// each shifter sweep a `window_ms` delay window and are crossfaded with sin² (gains sum to 1); the two shifters'
// crossfades are staggered. Mono in → stereo out; the dry passes with zero latency.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Microshift final : public Node {
public:
    enum { kCents, kDelay, kWindow, kMix, kSpread };
    static constexpr ParamSpec kParams[] = {
        {"cents", "ct", 0, 50, 9, Smooth::kLinear, "detune of each voice (left up, right down)"},
        {"delay_ms", "ms", 0, 30, 8, Smooth::kLinear, "extra delay of the voices (right gets 1.3x)"},
        {"window_ms", "ms", 10, 60, 30, Smooth::kFixed, "shifter sweep window (longer = smoother)"},
        {"mix", "", 0, 1, 0.3f, Smooth::kLinear, "level of the detuned voices added to the dry"},
        {"spread", "", 0, 1, 1, Smooth::kLinear, "1 = voices hard L/R, 0 = both centred"},
    };
    static constexpr int kCost = 30;
    static int outChannels(int) { return 2; }

    void prepare(Prepare& pr) override {
        w_ = static_cast<float>(p(kWindow) * pr.sample_rate * 0.001);
        const uint32_t max_d = static_cast<uint32_t>(w_ + 1.3 * 30.0 * pr.sample_rate * 0.001) + 8;
        const uint32_t n = dsp::DelayLine::sizeFor(max_d);
        line_.attach(pr.arena.alloc<float>(n), n);
    }
    void reset() noexcept override {
        line_.reset();
        ph_[0] = 0;
        ph_[1] = 0.25f;
        mix_.reset();
        spread_.reset();
    }
    void control() noexcept override {
        const double c = p(kCents) / 1200.0 * dsp::kLn2;
        dph_[0] = static_cast<float>((1.0 - dsp::dexp(c)) / w_);    // up: the delay shrinks
        dph_[1] = static_cast<float>((1.0 - dsp::dexp(-c)) / w_);   // down: the delay grows
        const float d = p(kDelay) * static_cast<float>(sampleRate()) * 0.001f;
        off_[0] = 2.0f + d;
        off_[1] = 2.0f + 1.3f * d;
        mix_.next(p(kMix));
        spread_.next(p(kSpread));
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (l[i] + r[i]) : l[i];
            line_.push(x);
            float v[2];
            for (int k = 0; k < 2; ++k) {
                ph_[k] += dph_[k];
                ph_[k] -= dsp::ffloor(ph_[k]);
                float ph2 = ph_[k] + 0.5f;
                if (ph2 >= 1.0f) ph2 -= 1.0f;
                const float s1 = dsp::sinTurns(0.5f * ph_[k]), s2 = dsp::sinTurns(0.5f * ph2);
                v[k] = line_.hermite(off_[k] + ph_[k] * w_) * (s1 * s1) + line_.hermite(off_[k] + ph2 * w_) * (s2 * s2);
            }
            const float mx = mix_.at(s.pos + i), sp = 0.5f * spread_.at(s.pos + i);
            const float a = (0.5f + sp) * mx, b = (0.5f - sp) * mx;   // amplitude pan: the mono sum keeps both voices
            const float dl = s.channels == 2 ? l[i] : x, dr = s.channels == 2 ? r[i] : x;
            l[i] = dl + a * v[0] + b * v[1];
            r[i] = dr + b * v[0] + a * v[1];
        }
    }

private:
    dsp::DelayLine line_;
    float w_ = 1440, ph_[2] = {0, 0.25f}, dph_[2] = {}, off_[2] = {2, 2};
    dsp::Ramp mix_, spread_;
};

FA_NODE(microshift, Microshift, "Stereo detune doubler (L a few cents up, R down), mono in → stereo out. Dry not delayed.")

}  // namespace fluent_audio
