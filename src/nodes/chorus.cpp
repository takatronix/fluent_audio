// chorus — 1..4 modulated delay voices (Hermite interpolation) spread over L/R; mono in → stereo out. Each voice
// has its own LFO phase and speed; `random` blends in smoothed noise (a doubler: voices 1, random 1). The dry
// passes untouched; only the wet is delayed.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Chorus final : public Node {
public:
    enum { kVoices, kRate, kDepth, kBase, kMix, kSpread, kRandom };
    static constexpr ParamSpec kParams[] = {
        {"voices", "", 1, 4, 2, Smooth::kFixed, "number of delayed voices"},
        {"rate_hz", "Hz", 0.01f, 5, 0.25f, Smooth::kLinear, "LFO speed"},
        {"depth_ms", "ms", 0, 8, 2.5f, Smooth::kLinear, "delay swing"},
        {"base_ms", "ms", 1, 30, 12, Smooth::kLinear, "centre delay"},
        {"mix", "", 0, 1, 0.3f, Smooth::kLinear, "wet amount"},
        {"spread", "", 0, 1, 1, Smooth::kLinear, "stereo spread of the voices"},
        {"random", "", 0, 1, 0, Smooth::kLinear, "0 = sine LFO, 1 = smoothed random wander"},
    };
    static constexpr int kCost = 40;
    static int outChannels(int) { return 2; }

    void prepare(Prepare& pr) override {
        const uint32_t max_d = static_cast<uint32_t>((30.0 + 8.0) * pr.sample_rate / 1000.0) + 8;
        const uint32_t n = dsp::DelayLine::sizeFor(max_d);
        line_.attach(pr.arena.alloc<float>(n), n);
        voices_ = static_cast<int>(p(kVoices));
        const double tick_rate = pr.sample_rate / kControlRate;
        hold_ = static_cast<int>(0.2 * tick_rate);
        nz_k_ = static_cast<float>(1.0 - dsp::poleMs(150.0, tick_rate));
    }
    void reset() noexcept override {
        line_.reset();
        rng_ = dsp::Rng(0x51A7u);
        for (int v = 0; v < 4; ++v) {
            phase_[v] = static_cast<float>(v) / static_cast<float>(voices_);
            noise_[v] = noise_prev_[v] = 0;
            d_[v] = -1;
        }
        for (int v = 0; v < 4; ++v) { target_[v] = 0; count_[v] = v * hold_ / 4; }
        mix_.reset();
    }
    void control() noexcept override {
        const float sr = static_cast<float>(sampleRate());
        for (int v = 0; v < voices_; ++v) {
            inc_[v] = p(kRate) * (1.0f + 0.13f * static_cast<float>(v)) / sr;
            const float pan = voices_ == 1 ? 0.0f
                                           : (static_cast<float>(v) / static_cast<float>(voices_ - 1) - 0.5f) * p(kSpread);
            gl_[v] = std::sqrt(0.5f - pan) / static_cast<float>(voices_);
            gr_[v] = std::sqrt(0.5f + pan) / static_cast<float>(voices_);
            // wander: a new random target every ~0.2 s (voices staggered), glided to with a one-pole
            if (++count_[v] >= hold_) { count_[v] = 0; target_[v] = rng_.uni(); }
            noise_prev_[v] = noise_[v];
            noise_[v] += (target_[v] - noise_[v]) * nz_k_;
        }
        base_ = p(kBase) * sr * 0.001f;
        depth_ = p(kDepth) * sr * 0.001f;
        rnd_ = p(kRandom);
        mix_.next(p(kMix));
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (l[i] + r[i]) : l[i];
            line_.push(x);
            const float k = static_cast<float>(s.pos + i + 1) * (1.0f / kControlRate);
            float wl = 0, wr = 0;
            for (int v = 0; v < voices_; ++v) {
                const float nz = noise_prev_[v] + (noise_[v] - noise_prev_[v]) * k;
                const float m = (1.0f - rnd_) * dsp::sinTurns(phase_[v]) + rnd_ * nz;
                phase_[v] += inc_[v];
                if (phase_[v] >= 1.0f) phase_[v] -= 1.0f;
                float want = base_ + depth_ * (0.5f + 0.5f * m);
                if (want < 2.0f) want = 2.0f;
                if (d_[v] < 0) d_[v] = want;
                const float step = dsp::clampf(want - d_[v], -0.5f, 0.5f);   // delay glides < 1 sample/sample
                d_[v] += step;
                const float y = line_.hermite(d_[v]);
                wl += y * gl_[v];
                wr += y * gr_[v];
            }
            const float mx = mix_.at(s.pos + i);
            const float dl = s.channels == 2 ? l[i] : x, dr = s.channels == 2 ? r[i] : x;
            l[i] = dl * (1.0f - 0.5f * mx) + mx * wl;
            r[i] = dr * (1.0f - 0.5f * mx) + mx * wr;
        }
    }

private:
    dsp::DelayLine line_;
    dsp::Rng rng_{0x51A7u};
    int voices_ = 2;
    float phase_[4] = {}, inc_[4] = {}, gl_[4] = {}, gr_[4] = {}, d_[4] = {-1, -1, -1, -1};
    float noise_[4] = {}, noise_prev_[4] = {}, target_[4] = {};
    int count_[4] = {}, hold_ = 300;
    float nz_k_ = 0.1f, base_ = 0, depth_ = 0, rnd_ = 0;
    dsp::Ramp mix_;
};

FA_NODE(chorus, Chorus, "Modulated-delay chorus / doubler, mono in → stereo out. Dry is not delayed.")

}  // namespace fluent_audio
