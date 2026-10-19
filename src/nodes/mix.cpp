// mix — "how much of this sound": out = dry·(1-a) + wet·a, linear (dry and wet are the same voice, design §5.1).
// The dry is the preset's dry tap (the signal after the parent chain, e.g. after voice_base).
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Mix final : public Node {
public:
    enum { kAmount };
    static constexpr ParamSpec kParams[] = {
        {"amount", "", 0, 1, 1, Smooth::kLinear, "0 = the dry tap, 1 = the whole effect"},
    };
    static constexpr int kCost = 2;

    void reset() noexcept override { a_.reset(); }
    void control() noexcept override { a_.next(p(kAmount)); }
    void process(Slice& s) noexcept override {
        if (!s.dry) return;
        for (int c = 0; c < s.channels; ++c)
            for (int i = 0; i < s.n; ++i) {
                const float a = a_.at(s.pos + i);
                s.ch[c][i] = s.dry[i] * (1.0f - a) + s.ch[c][i] * a;
            }
    }

private:
    dsp::Ramp a_;
};

FA_NODE(mix, Mix, "Wet/dry of the whole preset against its dry tap (linear).")

}  // namespace fluent_audio
