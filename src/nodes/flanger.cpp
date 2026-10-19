// flanger — the DJ "jet": one short delay per ear (min_ms..max_ms) swept by a slow LFO (sine → triangle by `shape`),
// fed back into itself (`feedback`, negative = hollow, positive = whistling), mixed with the dry so the comb
// notches sweep through the whole spectrum. The right ear's LFO runs `stereo` turns ahead. Mono in → stereo out;
// the dry is never delayed (zero latency). Made for the SYSTEM look (the user: 「フランジャーもっとDJぽく」).
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Flanger final : public Node {
public:
    enum { kRate, kMin, kMax, kFeedback, kMix, kStereo, kShape };
    static constexpr ParamSpec kParams[] = {
        {"rate_hz", "Hz", 0.02f, 5, 0.2f, Smooth::kLog, "sweep speed"},
        {"min_ms", "ms", 0.1f, 5, 0.3f, Smooth::kLinear, "shortest delay (top of the sweep)"},
        {"max_ms", "ms", 0.5f, 15, 4, Smooth::kLinear, "longest delay (bottom of the sweep)"},
        {"feedback", "", -0.95f, 0.95f, 0.7f, Smooth::kLinear, "resonance: + whistles, - hollow"},
        {"mix", "", 0, 1, 0.5f, Smooth::kLinear, "wet amount (0.5 = deepest notches)"},
        {"stereo", "turns", 0, 0.5f, 0.25f, Smooth::kLinear, "right LFO phase offset"},
        {"shape", "", 0, 1, 1, Smooth::kLinear, "0 = sine sweep, 1 = triangle (the classic jet)"},
    };
    static constexpr int kCost = 30;
    static int outChannels(int) { return 2; }

    void prepare(Prepare& pr) override {
        const uint32_t max_d = static_cast<uint32_t>(16.0 * pr.sample_rate / 1000.0) + 8;
        const uint32_t n = dsp::DelayLine::sizeFor(max_d);
        for (int c = 0; c < 2; ++c) line_[c].attach(pr.arena.alloc<float>(n), n);
    }
    void reset() noexcept override {
        for (int c = 0; c < 2; ++c) { line_[c].reset(); fb_[c] = 0; d_[c] = -1; }
        phase_ = 0;
        mix_.reset();
    }
    void control() noexcept override {
        const float sr = static_cast<float>(sampleRate());
        inc_ = p(kRate) / sr;
        lo_ = p(kMin) * sr * 0.001f;
        hi_ = (p(kMax) > p(kMin) ? p(kMax) : p(kMin)) * sr * 0.001f;
        fbk_ = p(kFeedback);
        st_ = p(kStereo);
        shape_ = p(kShape);
        mix_.next(p(kMix));
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (l[i] + r[i]) : l[i];
            const float mx = mix_.at(s.pos + i);
            float out[2];
            for (int c = 0; c < 2; ++c) {
                float t = phase_ + (c ? st_ : 0.0f);
                if (t >= 1.0f) t -= 1.0f;
                const float sine = 0.5f + 0.5f * dsp::sinTurns(t);
                const float tri = t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
                const float m = (1.0f - shape_) * sine + shape_ * tri;
                // the delay sweeps log-ish (more time at the short end, where the jet sings)
                float want = lo_ + (hi_ - lo_) * m * m;
                if (want < 2.0f) want = 2.0f;
                if (d_[c] < 0) d_[c] = want;
                d_[c] += dsp::clampf(want - d_[c], -0.5f, 0.5f);
                line_[c].push(dsp::flush(x + fbk_ * fb_[c]));
                const float y = line_[c].hermite(d_[c]);
                fb_[c] = dsp::clampf(y, -4.0f, 4.0f);
                const float dry = s.channels == 2 ? (c ? r[i] : l[i]) : x;
                out[c] = (1.0f - mx) * dry + mx * y;
            }
            phase_ += inc_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            l[i] = out[0];
            r[i] = out[1];
        }
    }

private:
    dsp::DelayLine line_[2];
    float fb_[2] = {}, d_[2] = {-1, -1};
    float phase_ = 0, inc_ = 0, lo_ = 0, hi_ = 0, fbk_ = 0, st_ = 0, shape_ = 1;
    dsp::Ramp mix_;
};

FA_NODE(flanger, Flanger, "DJ jet flanger: a short swept delay with feedback per ear, mono in → stereo out.")

}  // namespace fluent_audio
