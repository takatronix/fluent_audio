// deesser — a dynamic high shelf (design §4): the detector is a high-passed copy of the voice; the cut is applied
// by one high-shelf biquad whose gain follows the detector (0 .. max_db). Nothing is split and summed back, so
// there is no phase colour while it works.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Deesser final : public Node {
public:
    enum { kFreq, kThreshold, kRatio, kMax };
    static constexpr ParamSpec kParams[] = {
        {"freq", "Hz", 2000, 12000, 6500, Smooth::kLog, "detector high-pass and shelf corner"},
        {"threshold_db", "dB", -60, 0, -30, Smooth::kLinear, "detector level where the cut starts"},
        {"ratio", ":1", 1, 10, 3, Smooth::kLinear, "cut per dB over threshold"},
        {"max_db", "dB", -24, 0, -8, Smooth::kLinear, "deepest cut"},
    };
    static constexpr int kCost = 20;

    void reset() noexcept override {
        det_.reset();
        st_[0].reset();
        st_[1].reset();
        env_ = 0;
        shelf_db_ = 0;
    }
    void prepare(Prepare&) override {
        aa_ = static_cast<float>(dsp::poleMs(1.0, sampleRate()));
        ar_ = static_cast<float>(dsp::poleMs(60.0, sampleRate()));
    }
    void control() noexcept override {
        if (changed(kFreq)) hp_ = dsp::rbj(dsp::BiquadType::kHp, p(kFreq), 0.707, 0, sampleRate());
        const double over = dsp::lin2db(static_cast<double>(env_) + 1e-12) - p(kThreshold);
        const double knee = 6.0, slope = 1.0 - 1.0 / p(kRatio);
        double gr = 0.0;
        if (2.0 * over >= knee) gr = over * slope;
        else if (2.0 * over > -knee) gr = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
        double db = -gr;
        if (db < p(kMax)) db = p(kMax);
        const double d = db - shelf_db_;
        if (changed(kFreq) || d > 0.01 || d < -0.01) {
            shelf_db_ = db;
            shelf_ = dsp::rbj(dsp::BiquadType::kHighShelf, p(kFreq), 0.707, db, sampleRate());
        }
    }
    void process(Slice& s) noexcept override {
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (s.ch[0][i] + s.ch[1][i]) : s.ch[0][i];
            float h = det_.run(hp_, x);
            h = h < 0 ? -h : h;
            env_ = dsp::flush(h + (h > env_ ? aa_ : ar_) * (env_ - h));
        }
        for (int c = 0; c < s.channels; ++c)
            for (int i = 0; i < s.n; ++i) s.ch[c][i] = st_[c].run(shelf_, s.ch[c][i]);
    }

private:
    dsp::BiquadCoef hp_, shelf_;
    dsp::BiquadState det_, st_[2];
    float env_ = 0, aa_ = 0, ar_ = 0;
    double shelf_db_ = 0;
};

FA_NODE(deesser, Deesser, "De-esser as a dynamic high shelf (no band split).")

}  // namespace fluent_audio
