// subharmonic — an octave under the voice that follows it exactly (the dbx 100 idea): the voice's low band
// (2nd-order low-pass at `band_hz`) flips the sign of a flip-flop at each upward zero crossing (re-armed only
// after the band dips below -20 % of its envelope); the band times that ±1 is a waveform at half the pitch,
// smoothed by a 4th-order low-pass at `lp_hz`. Added to the untouched dry — no latency, filters only. Mono out.
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Subharmonic final : public Node {
public:
    enum { kBand, kLp, kAmount };
    static constexpr ParamSpec kParams[] = {
        {"band_hz", "Hz", 100, 600, 260, Smooth::kFixed, "low-pass that isolates the voice's fundamental"},
        {"lp_hz", "Hz", 40, 400, 140, Smooth::kFixed, "low-pass on the sub-octave"},
        {"amount", "", 0, 2, 0.5f, Smooth::kLinear, "level of the sub-octave added to the dry"},
    };
    static constexpr int kCost = 30;

    void prepare(Prepare& pr) override {
        cb_ = dsp::rbj(dsp::BiquadType::kLp, p(kBand), 0.707, 0, pr.sample_rate);
        cl_[0] = dsp::rbj(dsp::BiquadType::kLp, p(kLp), 0.5412, 0, pr.sample_rate);
        cl_[1] = dsp::rbj(dsp::BiquadType::kLp, p(kLp), 1.3066, 0, pr.sample_rate);
        a_env_ = static_cast<float>(dsp::poleMs(20, pr.sample_rate));
    }
    void reset() noexcept override {
        band_.reset();
        lp_[0].reset();
        lp_[1].reset();
        env_ = 0;
        armed_ = true;
        q_ = 1;
        prev_ = 0;
        amt_.reset();
    }
    void control() noexcept override { amt_.next(p(kAmount)); }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (l[i] + s.ch[1][i]) : l[i];
            const float b = band_.run(cb_, x);
            const float ab = b < 0 ? -b : b;
            env_ = dsp::flush(ab + a_env_ * (env_ - ab));
            if (armed_ && b > 0 && prev_ <= 0) {   // an upward zero crossing: flip (the product stays continuous)
                armed_ = false;
                q_ = -q_;
            } else if (!armed_ && b < -0.2f * env_) {
                armed_ = true;
            }
            prev_ = b;
            const float sub = lp_[1].run(cl_[1], lp_[0].run(cl_[0], b * q_));
            const float d = amt_.at(s.pos + i) * sub;
            l[i] += d;
            if (s.channels == 2) s.ch[1][i] += d;
        }
    }

private:
    dsp::BiquadCoef cb_, cl_[2];
    dsp::BiquadState band_, lp_[2];
    float a_env_ = 0, env_ = 0, q_ = 1, prev_ = 0;
    bool armed_ = true;
    dsp::Ramp amt_;
};

FA_NODE(subharmonic, Subharmonic, "Sub-octave that tracks the voice (flip-flop divider on its low band), added to the dry. No latency.")

}  // namespace fluent_audio
