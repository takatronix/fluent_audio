// choir — one voice becomes many: `voices` Doppler shifters (as microshift), detuned evenly across ±`cents`, each
// a little later (up to `delay_ms`) and drifting slowly on its own (`drift`, 0.05-0.2 Hz), spread across the
// stereo field and darkened (`tone_hz`). The dry passes untouched and undelayed (`dry` 0 = the choir only, for a
// bus). Mono in → stereo out.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Choir final : public Node {
public:
    enum { kVoices, kCents, kDelay, kDrift, kTone, kSpread, kMix, kDry };
    static constexpr ParamSpec kParams[] = {
        {"voices", "", 2, 8, 6, Smooth::kFixed, "number of copies"},
        {"cents", "ct", 0, 40, 10, Smooth::kLinear, "detune of the outermost copies (evenly spaced between)"},
        {"delay_ms", "ms", 0, 40, 14, Smooth::kFixed, "delay of the latest copy (the others between 20 % and 100 %)"},
        {"drift", "", 0, 1, 0.3f, Smooth::kLinear, "slow wander of each copy's detune (fraction of `cents`)"},
        {"tone_hz", "Hz", 1000, 16000, 7000, Smooth::kLog, "low-pass on the copies"},
        {"spread", "", 0, 1, 0.8f, Smooth::kLinear, "stereo spread of the copies"},
        {"mix", "", 0, 1, 0.35f, Smooth::kLinear, "level of the copies (their sum, power-normalised)"},
        {"dry", "", 0, 1, 1, Smooth::kLinear, "level of the untouched input"},
    };
    static constexpr int kCost = 120;
    static int outChannels(int) { return 2; }
    static constexpr int V = 8;
    static constexpr float kWinMs = 40;

    void prepare(Prepare& pr) override {
        n_ = static_cast<int>(p(kVoices) + 0.5f);
        w_ = static_cast<float>(kWinMs * pr.sample_rate * 0.001);
        const float d = static_cast<float>(p(kDelay) * pr.sample_rate * 0.001);
        for (int k = 0; k < V; ++k) {
            const float t = n_ > 1 ? static_cast<float>(k) / static_cast<float>(n_ - 1) : 0.5f;   // 0..1 across the copies
            pos_[k] = 2 * t - 1;                                                                  // detune / pan slot
            // delays interleaved so neighbours in pitch are not neighbours in time
            const float dt = 0.2f + 0.8f * static_cast<float>((k * 5) % V) / (V - 1);
            off_[k] = 2.0f + d * dt;
            rate_[k] = static_cast<float>((0.05 + 0.021 * k) * kControlRate / pr.sample_rate);
        }
        const uint32_t n = dsp::DelayLine::sizeFor(static_cast<uint32_t>(w_ + d) + 8);
        line_.attach(pr.arena.alloc<float>(n), n);
        norm_ = static_cast<float>(1.0 / std::sqrt(static_cast<double>(n_)));
    }
    void reset() noexcept override {
        line_.reset();
        for (int k = 0; k < V; ++k) {
            ph_[k] = static_cast<float>(k) / V;
            lfo_[k] = static_cast<float>((k * 3) % V) / V;
            lp_[k].reset();
        }
        mix_.reset();
        dry_.reset();
        spread_.reset();
    }
    void control() noexcept override {
        const float cents = p(kCents), drift = p(kDrift);
        for (int k = 0; k < n_; ++k) {
            const double c = (cents * pos_[k] + 0.5f * drift * cents * dsp::sinTurns(lfo_[k])) / 1200.0 * dsp::kLn2;
            dph_[k] = static_cast<float>((1.0 - dsp::dexp(c)) / w_);
            lfo_[k] += rate_[k];
            if (lfo_[k] >= 1.0f) lfo_[k] -= 1.0f;
        }
        if (changed(kTone)) a_ = static_cast<float>(dsp::poleHz(p(kTone), sampleRate()));
        mix_.next(p(kMix) * norm_);
        dry_.next(p(kDry));
        spread_.next(p(kSpread));
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (l[i] + r[i]) : l[i];
            line_.push(x);
            const float sp = spread_.at(s.pos + i);
            float yl = 0, yr = 0;
            for (int k = 0; k < n_; ++k) {
                float ph = ph_[k] + dph_[k];
                ph -= dsp::ffloor(ph);
                ph_[k] = ph;
                float ph2 = ph + 0.5f;
                if (ph2 >= 1.0f) ph2 -= 1.0f;
                const float s1 = dsp::sinTurns(0.5f * ph), s2 = dsp::sinTurns(0.5f * ph2);
                const float v = lp_[k].run(a_, line_.hermite(off_[k] + ph * w_) * (s1 * s1) +
                                                   line_.hermite(off_[k] + ph2 * w_) * (s2 * s2));
                const float pan = sp * pos_[(k + n_ / 2) % n_];   // pan order rotated against pitch order
                yl += v * (1.0f - pan);
                yr += v * (1.0f + pan);
            }
            const float mx = 0.5f * mix_.at(s.pos + i), dr = dry_.at(s.pos + i);
            const float dl = s.channels == 2 ? l[i] : x, drr = s.channels == 2 ? r[i] : x;
            l[i] = dr * dl + mx * yl;
            r[i] = dr * drr + mx * yr;
        }
    }

private:
    dsp::DelayLine line_;
    int n_ = 6;
    float w_ = 1920, norm_ = 0.4f, a_ = 0;
    float pos_[V] = {}, off_[V] = {}, ph_[V] = {}, dph_[V] = {}, lfo_[V] = {}, rate_[V] = {};
    dsp::OnePole lp_[V];
    dsp::Ramp mix_, dry_, spread_;
};

FA_NODE(choir, Choir, "Ensemble: several slowly drifting detuned, delayed copies spread wide. Mono in → stereo out, dry undelayed.")

}  // namespace fluent_audio
