// gain — a level change in dB, smoothed per sample. Also the plain receiver for a $param ("gain: { db: $params.x }").
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Gain final : public Node {
public:
    enum { kDb };
    static constexpr ParamSpec kParams[] = {
        {"db", "dB", -60, 24, 0, Smooth::kLinear, "gain"},
    };
    static constexpr int kCost = 1;

    void reset() noexcept override { g_.reset(); }
    void control() noexcept override { g_.next(static_cast<float>(dsp::db2lin(p(kDb)))); }
    void process(Slice& s) noexcept override {
        for (int c = 0; c < s.channels; ++c)
            for (int i = 0; i < s.n; ++i) s.ch[c][i] *= g_.at(s.pos + i);
    }

private:
    dsp::Ramp g_;
};

FA_NODE(gain, Gain, "Level change in dB (smoothed per sample).")

}  // namespace fluent_audio
