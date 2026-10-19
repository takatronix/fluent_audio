// send — copy the signal at this point of the chain into a bus (`to:`), scaled by `gain`. The chain itself is
// unchanged. Buses are wet-only branches summed with the chain before the master section.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Send final : public Node {
public:
    enum { kGain };
    static constexpr ParamSpec kParams[] = {
        {"gain", "", 0, 4, 0.1f, Smooth::kLinear, "linear send level"},
    };
    static constexpr int kCost = 2;

    void connectBus(BusInput* bus) override { bus_ = bus; }
    void reset() noexcept override { g_.reset(); }
    void control() noexcept override { g_.next(p(kGain)); }
    void process(Slice& s) noexcept override {
        if (!bus_) return;
        for (int c = 0; c < bus_->channels; ++c) {
            const float* x = s.ch[s.channels == 2 ? c : 0];
            float* y = bus_->ch[c];
            for (int i = 0; i < s.n; ++i) y[i] += x[i] * g_.at(s.pos + i);
        }
    }

private:
    BusInput* bus_ = nullptr;
    dsp::Ramp g_;
};

FA_NODE(send, Send, "Branch into a bus (`to: <bus>`), wet only.")

}  // namespace fluent_audio
