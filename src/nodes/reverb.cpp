// reverb — 8-line FDN (design §4). Householder feedback A = I - (2/N)·11ᵀ (orthogonal); per line a gain for the
// RT60 and a one-pole damping low-pass (DC gain 1), so the loop gain is < 1 at every frequency. The line lengths
// wander slowly (≤ 8 samples, < 1 Hz, linear interpolation) to break up metallic ringing. Input: predelay, then
// 4 series allpasses for diffusion. Output: stereo from two orthogonal taps; `mix` 1 = wet only (use in a bus).
#include "fluent_audio/node.hpp"

namespace fluent_audio {

class Reverb final : public Node {
public:
    enum { kDecay, kSize, kDamp, kPredelay, kDiffusion, kMod, kWidth, kMix };
    static constexpr ParamSpec kParams[] = {
        {"decay_s", "s", 0.1f, 20, 1.2f, Smooth::kLinear, "RT60"},
        {"size", "", 0.2f, 2.5f, 1, Smooth::kFixed, "scales the delay lines"},
        {"damp_hz", "Hz", 500, 20000, 6000, Smooth::kLog, "high frequencies die faster above this"},
        {"predelay_ms", "ms", 0, 250, 10, Smooth::kFixed, "gap before the reverb starts"},
        {"diffusion", "", 0, 0.9f, 0.7f, Smooth::kLinear, "input allpass gain (smear)"},
        {"mod", "", 0, 1, 0.2f, Smooth::kLinear, "delay-line wander (0..8 samples)"},
        {"width", "", 0, 1, 1, Smooth::kLinear, "stereo width of the tail"},
        {"mix", "", 0, 1, 1, Smooth::kLinear, "1 = wet only (buses), < 1 mixes the dry back"},
    };
    static constexpr int kCost = 80;
    static int outChannels(int) { return 2; }
    static constexpr int N = 8;

    void prepare(Prepare& pr) override {
        static const int kBase[N] = {1031, 1327, 1523, 1801, 2053, 2311, 2633, 2897};
        static const int kAp[4] = {142, 107, 379, 277};
        const double scale = pr.sample_rate / 48000.0;
        for (int i = 0; i < N; ++i) {
            len_[i] = static_cast<float>(static_cast<int>(kBase[i] * p(kSize) * scale + 0.5));
            if (len_[i] < 40.0f) len_[i] = 40.0f;
            const uint32_t n = dsp::DelayLine::sizeFor(static_cast<uint32_t>(len_[i]) + 10);
            line_[i].attach(pr.arena.alloc<float>(n), n);
            lfo_inc_[i] = static_cast<float>((0.1 + 0.11 * i) * kControlRate / pr.sample_rate);   // per tick
        }
        for (int k = 0; k < 4; ++k) {
            ap_len_[k] = static_cast<uint32_t>(kAp[k] * scale + 0.5);
            const uint32_t n = dsp::DelayLine::sizeFor(ap_len_[k]);
            ap_[k].attach(pr.arena.alloc<float>(n), n);
        }
        pd_ = static_cast<uint32_t>(p(kPredelay) * pr.sample_rate * 0.001 + 0.5);
        if (pd_ > 0) {
            const uint32_t n = dsp::DelayLine::sizeFor(pd_);
            pre_.attach(pr.arena.alloc<float>(n), n);
        }
    }
    void reset() noexcept override {
        for (int i = 0; i < N; ++i) {
            line_[i].reset();
            lp_[i].reset();
            lfo_[i] = static_cast<float>(i) / N;
            off_[i] = off_prev_[i] = 0;
        }
        for (auto& a : ap_) a.reset();
        if (pd_) pre_.reset();
        mix_.reset();
        width_.reset();
    }
    void control() noexcept override {
        const double sr = sampleRate();
        if (changed(kDecay) || changed(kSize)) {
            for (int i = 0; i < N; ++i)
                g_[i] = static_cast<float>(dsp::dexp(-3.0 * dsp::kLn10 * len_[i] / (sr * p(kDecay))));
        }
        if (changed(kDamp)) damp_ = static_cast<float>(dsp::poleHz(p(kDamp), sr));
        const float depth = 8.0f * p(kMod);
        for (int i = 0; i < N; ++i) {
            off_prev_[i] = off_[i];
            off_[i] = depth * (0.5f + 0.5f * dsp::sinTurns(lfo_[i]));
            lfo_[i] += lfo_inc_[i];
            if (lfo_[i] >= 1.0f) lfo_[i] -= 1.0f;
        }
        diff_ = p(kDiffusion);
        mix_.next(p(kMix));
        width_.next(p(kWidth));
    }
    void process(Slice& s) noexcept override {
        static const float kSl[N] = {1, -1, 1, -1, 1, -1, 1, -1};
        static const float kSr[N] = {1, 1, -1, -1, 1, 1, -1, -1};
        float* l = s.ch[0];
        float* r = s.ch[1];
        // locals: stores into the delay lines (float*) could alias float members, which would force reloads
        const float damp = damp_, diff = diff_;
        float len[N], g[N], o0[N], od[N];
        for (int j = 0; j < N; ++j) { len[j] = len_[j]; g[j] = g_[j]; o0[j] = off_prev_[j]; od[j] = off_[j] - off_prev_[j]; }
        for (int i = 0; i < s.n; ++i) {
            const float dl = l[i], dr = s.channels == 2 ? r[i] : l[i];
            float x = s.channels == 2 ? 0.5f * (dl + dr) : dl;
            if (pd_) { pre_.push(x); x = pre_.tap(pd_ + 1); }
            for (int k = 0; k < 4; ++k) {   // Schroeder allpasses: |H| = 1
                const float d = ap_[k].tap(ap_len_[k]);
                const float u = x + diff * d;
                x = d - diff * u;
                ap_[k].push(u);
            }
            const float k = static_cast<float>(s.pos + i + 1) * (1.0f / kControlRate);
            float y[N];
            float sum = 0;
            for (int j = 0; j < N; ++j) {
                const float off = o0[j] + od[j] * k;
                y[j] = lp_[j].run(damp, line_[j].linear(len[j] - off)) * g[j];
                sum += y[j];
            }
            sum *= 2.0f / N;
            const float in = x * 0.35f;
            float wl = 0, wr = 0;
            for (int j = 0; j < N; ++j) {
                line_[j].push(in + (y[j] - sum));
                wl += kSl[j] * y[j];
                wr += kSr[j] * y[j];
            }
            wl *= 0.5f;
            wr *= 0.5f;
            const float m = 0.5f * (wl + wr), sd = 0.5f * (wl - wr) * width_.at(s.pos + i);
            const float mx = mix_.at(s.pos + i);
            l[i] = dl * (1.0f - mx) + (m + sd) * mx;
            r[i] = dr * (1.0f - mx) + (m - sd) * mx;
        }
    }

private:
    dsp::DelayLine line_[N], ap_[4], pre_;
    dsp::OnePole lp_[N];
    float len_[N] = {}, g_[N] = {}, lfo_[N] = {}, lfo_inc_[N] = {}, off_[N] = {}, off_prev_[N] = {};
    uint32_t ap_len_[4] = {}, pd_ = 0;
    float damp_ = 0, diff_ = 0.7f;
    dsp::Ramp mix_, width_;
};

FA_NODE(reverb, Reverb, "8-line FDN reverb (Householder, damped, modulated). Mono/stereo in → stereo out.")

}  // namespace fluent_audio
