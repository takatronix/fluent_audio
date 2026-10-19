// dilate — time slows at the end of a phrase: when the voice stops (dsp::SpeechGate, 180 ms hold), the last
// `hold_ms` of it is frozen and played on by 4 overlapping Hann grains from random places in that stretch, the
// pitch sinking by `glide_st` and the level falling 60 dB over `decay_s` — the last syllable hangs and sinks
// instead of ending. When the voice comes back the frozen tail ducks out in ~30 ms. Silent while speaking, so
// it never smears the words; feed it into a reverb. Output: `dry`·input + `level`·tail (mono).
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Dilate final : public Node {
public:
    enum { kHold, kGrain, kDecay, kGlide, kLevel, kDry };
    static constexpr ParamSpec kParams[] = {
        {"hold_ms", "ms", 80, 600, 250, Smooth::kFixed, "length of the frozen stretch (the end of the phrase)"},
        {"grain_ms", "ms", 30, 250, 120, Smooth::kFixed, "grain length"},
        {"decay_s", "s", 0.5f, 12, 4, Smooth::kFixed, "time the frozen tail takes to fall 60 dB"},
        {"glide_st", "st", -12, 0, -2, Smooth::kFixed, "pitch the tail sinks by over decay_s"},
        {"level", "", 0, 2, 0.7f, Smooth::kLinear, "level of the frozen tail"},
        {"dry", "", 0, 1, 0, Smooth::kLinear, "level of the input passed through (0 for a bus)"},
    };
    static constexpr int kCost = 60;
    static constexpr int G = 4, kFar = 1 << 20;

    void prepare(Prepare& pr) override {
        const double sr = pr.sample_rate;
        gate_.setup(sr, -30, 180);
        hold_ = static_cast<float>(p(kHold) * sr * 0.001);
        glen_ = static_cast<int>(p(kGrain) * sr * 0.001);
        // the farthest read: end of the stretch (<= 0.3 s back) + hold_ms (<= 0.6 s) + the duck (~0.35 s)
        const uint32_t n = dsp::DelayLine::sizeFor(static_cast<uint32_t>(1.3 * sr));
        max_end_ = static_cast<float>(0.3 * sr);
        line_.attach(pr.arena.alloc<float>(n), n);
        a_dec_ = static_cast<float>(dsp::poleMs(p(kDecay) * 1000.0 / 6.9078, sr));
        a_in_ = static_cast<float>(dsp::poleMs(15, sr));
        a_duck_ = static_cast<float>(dsp::poleMs(30, sr));
        len_ticks_ = p(kDecay) * sr / kControlRate;
    }
    void reset() noexcept override {
        line_.reset();
        gate_.reset();
        state_ = kIdle;
        spoke_ = false;
        since_loud_ = 0;
        fade_ = dec_ = 0;
        ticks_ = 0;
        rate_ = 1;
        rng_ = dsp::Rng(0x5EED1234u);
        for (int g = 0; g < G; ++g) { d_[g] = 0; age_[g] = 0; }
        level_.reset();
        dry_.reset();
    }
    void control() noexcept override {
        if (state_ == kFrozen) {
            const double t = ticks_ < len_ticks_ ? ticks_ / len_ticks_ : 1.0;
            rate_ = static_cast<float>(dsp::dexp(p(kGlide) * t / 12.0 * dsp::kLn2));
            ++ticks_;
        }
        level_.next(p(kLevel) * 0.5f);   // 4 Hann grains at 1/4 overlap sum to 2
        dry_.next(p(kDry));
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (l[i] + s.ch[1][i]) : l[i];
            const bool on = gate_.run(x);
            since_loud_ = gate_.env > 0.25f * gate_.peak ? 0 : (since_loud_ < kFar ? since_loud_ + 1 : kFar);
            if (on) {
                spoke_ = true;
                if (state_ == kFrozen) state_ = kDuck;
            } else if (spoke_ && state_ != kFrozen) {
                freeze();
            }
            float y = 0;
            if (state_ == kFrozen) {
                fade_ = 1.0f + a_in_ * (fade_ - 1.0f);
                dec_ *= a_dec_;
            } else {
                line_.push(x);
                if (state_ == kDuck) {
                    dec_ *= a_duck_;
                    for (int g = 0; g < G; ++g) d_[g] += 1.0f;   // the line moved on under the grains
                    d_end_ += 1.0f;
                }
            }
            if (state_ != kIdle && dec_ < 1e-5f) { state_ = kIdle; dec_ = 0; }
            if (state_ != kIdle) {
                for (int g = 0; g < G; ++g) {
                    if (age_[g] >= glen_) restart(g);
                    const float w = dsp::sinTurns(0.5f * static_cast<float>(age_[g]) / static_cast<float>(glen_));
                    y += line_.hermite(d_[g]) * (w * w);
                    d_[g] -= rate_;
                    ++age_[g];
                }
                y *= fade_ * dec_ * level_.at(s.pos + i);
            }
            const float dr = dry_.at(s.pos + i);
            l[i] = (s.channels == 2 ? dr * l[i] : dr * x) + y;
            if (s.channels == 2) s.ch[1][i] = dr * s.ch[1][i] + y;
        }
    }

private:
    enum State { kIdle, kFrozen, kDuck };
    void freeze() {
        state_ = kFrozen;
        spoke_ = false;
        fade_ = 0;
        dec_ = 1;
        ticks_ = 0;
        rate_ = 1;
        d_end_ = static_cast<float>(since_loud_) + 2.0f;  // the stretch ends where the voice was last loud
        if (d_end_ > max_end_) d_end_ = max_end_;
        for (int g = 0; g < G; ++g) {
            restart(g);
            age_[g] = g * glen_ / G;                       // staggered
            d_[g] -= rate_ * static_cast<float>(age_[g]);
        }
    }
    void restart(int g) {
        // start anywhere in the frozen stretch that leaves room for the whole grain
        const float span = hold_ - static_cast<float>(glen_);
        const float u = 0.5f + 0.5f * rng_.uni();
        d_[g] = d_end_ + static_cast<float>(glen_) * rate_ + (span > 0 ? u * span : 0.0f);
        age_[g] = 0;
    }

    dsp::DelayLine line_;
    dsp::SpeechGate gate_;
    dsp::Rng rng_{0x5EED1234u};
    State state_ = kIdle;
    bool spoke_ = false;
    float hold_ = 12000, max_end_ = 14400, a_dec_ = 0, a_in_ = 0, a_duck_ = 0, fade_ = 0, dec_ = 0, rate_ = 1, d_end_ = 2;
    float d_[G] = {};
    int glen_ = 5760, age_[G] = {}, since_loud_ = 0;
    double ticks_ = 0, len_ticks_ = 1;
    dsp::Ramp level_, dry_;
};

FA_NODE(dilate, Dilate, "Time dilation: the end of each phrase frozen into slow grains that sink in pitch and fade. Silent while speaking.")

}  // namespace fluent_audio
