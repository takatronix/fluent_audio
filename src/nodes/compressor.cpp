// compressor — feed-forward, soft knee, log-domain gain computer. The envelope runs per sample (linked
// channels); the gain is computed per control tick and interpolated per sample.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Compressor final : public Node {
public:
    enum { kThreshold, kRatio, kAttack, kRelease, kKnee, kMakeup };
    static constexpr ParamSpec kParams[] = {
        {"threshold_db", "dB", -60, 0, -24, Smooth::kLinear, "level where compression starts"},
        {"ratio", ":1", 1, 20, 2, Smooth::kLinear, "input dB over threshold per output dB"},
        {"attack_ms", "ms", 0.1f, 200, 10, Smooth::kLinear, "envelope attack"},
        {"release_ms", "ms", 5, 2000, 150, Smooth::kLinear, "envelope release"},
        {"knee_db", "dB", 0, 24, 6, Smooth::kLinear, "soft-knee width"},
        {"makeup_db", "dB", -12, 24, 0, Smooth::kLinear, "gain after compression"},
    };
    static constexpr int kCost = 15;

    void reset() noexcept override { env_ = 0; g_.reset(); }
    void control() noexcept override {
        if (changed(kAttack) || changed(kRelease)) {
            aa_ = static_cast<float>(dsp::poleMs(p(kAttack), sampleRate()));
            ar_ = static_cast<float>(dsp::poleMs(p(kRelease), sampleRate()));
        }
        const double lvl = dsp::lin2db(static_cast<double>(env_) + 1e-12);
        const double over = lvl - p(kThreshold), knee = p(kKnee), slope = 1.0 - 1.0 / p(kRatio);
        double gr;
        if (2.0 * over <= -knee) gr = 0.0;
        else if (2.0 * over >= knee) gr = over * slope;
        else gr = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
        g_.next(static_cast<float>(dsp::db2lin(p(kMakeup) - gr)));
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            float a = l[i] < 0 ? -l[i] : l[i];
            if (s.channels == 2) { const float b = r[i] < 0 ? -r[i] : r[i]; if (b > a) a = b; }
            env_ = dsp::flush(a + (a > env_ ? aa_ : ar_) * (env_ - a));
            const float g = g_.at(s.pos + i);
            l[i] *= g;
            if (s.channels == 2) r[i] *= g;
        }
    }

private:
    float env_ = 0, aa_ = 0, ar_ = 0;
    dsp::Ramp g_;
};

FA_NODE(compressor, Compressor, "Feed-forward soft-knee compressor (zero latency).")

}  // namespace fluent_audio
