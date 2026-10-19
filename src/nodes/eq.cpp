// eq — up to 8 RBJ biquads in series (TDF-II transposed). Coefficients are recomputed at control rate while a
// band's f / q / gain moves (frequencies glide in the log domain), so sweeps stay smooth and stable.
//   - eq: { bands: [ { type: hp, f: 80, q: 0.7 }, { type: highshelf, f: 9000, gain_db: 3 } ] }
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Eq final : public Node {
public:
    enum { kType, kF, kQ, kGain };
    static constexpr ParamSpec kParams[] = {
        {"type", "", 0, 7, 3, Smooth::kFixed, "filter shape", dsp::kBiquadChoices, "bands"},
        {"f", "Hz", 20, 20000, 1000, Smooth::kLog, "centre / corner frequency", nullptr, "bands"},
        {"q", "", 0.1f, 18, 0.707f, Smooth::kLog, "resonance (0.707 = Butterworth)", nullptr, "bands"},
        {"gain_db", "dB", -24, 24, 0, Smooth::kLinear, "gain of peak / shelf bands", nullptr, "bands"},
    };
    static constexpr int kMaxItems = 8;
    static constexpr int kCost = 15;

    void reset() noexcept override {
        for (auto& band : st_)
            for (auto& ch : band) ch.reset();
    }
    void control() noexcept override {
        if (!changed()) return;
        for (int b = 0; b < items(); ++b)
            if (changed(kF, b) || changed(kQ, b) || changed(kGain, b) || changed(kType, b))
                c_[b] = dsp::rbj(static_cast<dsp::BiquadType>(static_cast<int>(p(kType, b))), p(kF, b), p(kQ, b),
                                 p(kGain, b), sampleRate());
    }
    void process(Slice& s) noexcept override {
        for (int c = 0; c < s.channels; ++c) {
            float* x = s.ch[c];
            for (int b = 0; b < items(); ++b) {
                dsp::BiquadState& st = st_[b][c];
                const dsp::BiquadCoef& k = c_[b];
                for (int i = 0; i < s.n; ++i) x[i] = st.run(k, x[i]);
            }
        }
    }

private:
    dsp::BiquadCoef c_[kMaxItems];
    dsp::BiquadState st_[kMaxItems][2];
};

FA_NODE(eq, Eq, "Parametric EQ: up to 8 biquad bands (hp, lp, bp, peak, lowshelf, highshelf, notch, allpass).")

}  // namespace fluent_audio
