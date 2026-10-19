// organ — a pipe-organ pad that plays under a voice: whenever someone speaks (dsp::SpeechGate on the input), a
// chord swells in on 16'/8'/4'/2⅔'/2' ranks (+ an 8' celeste a few cents sharp, which beats slowly), as loud as
// the voice's slow RMS; when the voice stops, the chord is held `sustain_s` more and then dies away over
// `release_s` (feed a long reverb after it). `chords: cycle` moves Am → F → C → G, one chord per phrase (a new
// phrase = dsp::SpeechGate::phrase, the voice back after ~0.3 s of silence: a sentence break in TTS). The old
// chord's pipes release while the new ones swell in, as an organist changes chords.
// Each pipe is a rotating phasor (cos, sin), renormalised every tick, shaped with Chebyshev T2/T3 into a flue
// tone; keys are spread across the stereo field like the pipes of a case. Output: the organ only (for a bus).
#include "fluent_audio/node.hpp"

namespace fluent_audio {
namespace organ_keys {
constexpr int K = 12, P = 6;                                                    // keys, pipes per key
constexpr int kKeys[K] = {-4, -2, 0, 3, 7, 8, 10, 12, 14, 15, 17, 19};          // semitones from A2
constexpr float kMul[P] = {0.5f, 1, 1, 2, 3, 4};                                // 16' 8' 8'celeste 4' 2⅔' 2'
constexpr uint16_t bit(int st) {
    uint16_t m = 0;
    for (int k = 0; k < K; ++k) if (kKeys[k] == st) m = static_cast<uint16_t>(1u << k);
    return m;
}
constexpr uint16_t kAm = bit(0) | bit(7) | bit(12) | bit(15);                   // A2 E3 A3 C4
constexpr uint16_t kF = bit(-4) | bit(8) | bit(12) | bit(15);                   // F2 F3 A3 C4
constexpr uint16_t kC = bit(3) | bit(10) | bit(15) | bit(19);                   // C3 G3 C4 E4
constexpr uint16_t kG = bit(-2) | bit(10) | bit(14) | bit(17);                  // G2 G3 B3 D4
constexpr uint16_t kDrone = bit(0) | bit(7) | bit(12);
}  // namespace organ_keys
using namespace organ_keys;

class Organ final : public Node {
public:
    enum { kNote, kChords, kR16, kR8, kR4, kQuint, kR2, kCeleste, kSwell, kSustain, kRelease, kTone, kSpread, kLevel };
    static constexpr ParamSpec kParams[] = {
        {"note", "st", -12, 12, 0, Smooth::kFixed, "key: 0 = A (8' at 110 Hz)"},
        {"chords", "", 0, 2, 2, Smooth::kFixed, "drone = root+5th, minor = one chord, cycle = i-VI-III-VII by phrase",
         "drone|minor|cycle"},
        {"r16", "", 0, 1, 0.6f, Smooth::kLinear, "16' rank (an octave below the 8')"},
        {"r8", "", 0, 1, 1, Smooth::kLinear, "8' rank (the key) and its celeste"},
        {"r4", "", 0, 1, 0.5f, Smooth::kLinear, "4' rank (an octave up)"},
        {"quint", "", 0, 1, 0.15f, Smooth::kLinear, "2 2/3' rank (a twelfth up)"},
        {"r2", "", 0, 1, 0.2f, Smooth::kLinear, "2' rank (two octaves up)"},
        {"celeste", "ct", 0, 20, 4, Smooth::kFixed, "detune of the second 8' (slow beating)"},
        {"swell_ms", "ms", 50, 3000, 700, Smooth::kFixed, "how slowly a chord swells in"},
        {"sustain_s", "s", 0, 3, 0.4f, Smooth::kFixed, "the chord is held this long after the voice stops"},
        {"release_s", "s", 0.2f, 8, 2.5f, Smooth::kFixed, "time the pipes take to fall 60 dB after the voice stops"},
        {"tone_hz", "Hz", 500, 12000, 3000, Smooth::kLog, "low-pass on the organ (darker below)"},
        {"spread", "", 0, 1, 0.6f, Smooth::kLinear, "stereo spread of the keys"},
        {"level", "", 0, 4, 1, Smooth::kLinear, "organ level relative to the voice's RMS (1 ≈ -15 dB under it)"},
    };
    static constexpr int kCost = 400;
    static int outChannels(int) { return 2; }

    static constexpr int K = organ_keys::K, P = organ_keys::P;

    void prepare(Prepare& pr) override {
        gate_.setup(pr.sample_rate, -30, 150);
        a_rms_ = static_cast<float>(dsp::poleMs(1000, pr.sample_rate));   // slow: the organ must not pump with syllables
        a_att_ = static_cast<float>(dsp::poleMs(p(kSwell), pr.sample_rate));
        a_rel_ = static_cast<float>(dsp::poleMs(p(kRelease) * 1000.0 / 6.9078, pr.sample_rate));   // T60 = 6.9 τ
        sustain_ = static_cast<int>(p(kSustain) * pr.sample_rate);

    }
    void reset() noexcept override {
        gate_.reset();
        for (int k = 0; k < K; ++k) {
            env_[k] = 0;
            for (int j = 0; j < P; ++j) { c_[k][j] = 1; s_[k][j] = 0; }
        }
        lp_[0][0].reset(); lp_[0][1].reset(); lp_[1][0].reset(); lp_[1][1].reset();
        ms_ = 0; loud_ = 0; off_for_ = 0; chord_ = -1;
        held_ = 0;
        for (auto* r : {&r16_, &r8_, &r4_, &rq_, &r2_, &spread_, &level_}) r->reset();
    }
    void control() noexcept override {
        const double sr = sampleRate();
        if (changed(kNote) || changed(kCeleste)) {
            const double cel = dsp::dexp(p(kCeleste) / 1200.0 * dsp::kLn2);
            for (int k = 0; k < K; ++k) {
                const double f = 110.0 * dsp::dexp((kKeys[k] + p(kNote)) / 12.0 * dsp::kLn2);
                for (int j = 0; j < P; ++j) {
                    const double hz = f * kMul[j] * (j == 2 ? cel : 1.0);
                    double sn, cs;
                    dsp::dsincos(2.0 * dsp::kPi * hz / sr, sn, cs);
                    sw_[k][j] = static_cast<float>(sn);
                    cw_[k][j] = static_cast<float>(cs);
                    ok_[k][j] = 3.0 * hz < 0.45 * sr ? 1.0f : 0.0f;          // T3 would alias: mute that pipe
                }
            }
        }
        if (changed(kTone)) a_tone_ = static_cast<float>(dsp::poleHz(p(kTone), sr));
        for (int k = 0; k < K; ++k) {                                       // keep each phasor on the unit circle
            if (env_[k] == 0) continue;
            for (int j = 0; j < P; ++j) {
                const float g = 1.5f - 0.5f * (c_[k][j] * c_[k][j] + s_[k][j] * s_[k][j]);
                c_[k][j] *= g;
                s_[k][j] *= g;
            }
        }
        loud_ = ms_ > 0 ? static_cast<float>(std::sqrt(static_cast<double>(ms_))) : 0.0f;
        r16_.next(p(kR16)); r8_.next(p(kR8)); r4_.next(p(kR4)); rq_.next(p(kQuint)); r2_.next(p(kR2));
        spread_.next(p(kSpread));
        level_.next(p(kLevel) * 0.25f);
    }
    void process(Slice& s) noexcept override {
        float* l = s.ch[0];
        float* r = s.ch[1];
        for (int i = 0; i < s.n; ++i) {
            const float x = s.channels == 2 ? 0.5f * (l[i] + r[i]) : l[i];
            const bool voice = gate_.run(x);
            const bool on = voice || (chord_ >= 0 && off_for_ < sustain_);   // keys held a little after the voice
            if (voice) {
                if (gate_.phrase) advance();                                  // a new phrase: the next chord
                ms_ = dsp::flush(x * x + a_rms_ * (ms_ - x * x));
                off_for_ = 0;
            } else if (off_for_ < sustain_) {
                ++off_for_;
            }
            const int pos = s.pos + i;
            const float g16 = r16_.at(pos), g8 = r8_.at(pos), g4 = r4_.at(pos), gq = rq_.at(pos), g2 = r2_.at(pos);
            const float gain[P] = {g16, g8, 0.7f * g8, g4, gq, g2};
            const float sp = spread_.at(pos), amp = level_.at(pos) * loud_;
            float yl = 0, yr = 0;
            for (int k = 0; k < K; ++k) {
                const bool key = on && (held_ >> k & 1u);
                float e = env_[k];
                if (key) e = 1.0f + a_att_ * (e - 1.0f);
                else if (e != 0) { e *= a_rel_; if (e < 1e-6f) e = 0; }
                env_[k] = e;
                if (e == 0) continue;
                float v = 0;
                for (int j = 0; j < P; ++j) {
                    const float c = c_[k][j], sn = s_[k][j];
                    const float cw = cw_[k][j], sw = sw_[k][j];
                    c_[k][j] = c * cw - sn * sw;
                    s_[k][j] = c * sw + sn * cw;
                    const float c2 = c * c;
                    v += gain[j] * ok_[k][j] * (c + 0.15f * (2.0f * c2 - 1.0f) + 0.06f * c * (4.0f * c2 - 3.0f));   // flue: T1 + T2 + T3
                }
                v *= e * amp;
                const float pan = sp * kPan[k];
                yl += v * (0.5f - 0.5f * pan);
                yr += v * (0.5f + 0.5f * pan);
            }
            l[i] = lp_[0][1].run(a_tone_, lp_[0][0].run(a_tone_, yl));
            r[i] = lp_[1][1].run(a_tone_, lp_[1][0].run(a_tone_, yr));
        }
    }

private:
    static constexpr float kPan[K] = {-0.2f, 0.2f, 0.0f, -0.5f, 0.5f, -0.6f, 0.6f, -0.3f, 0.8f, 0.3f, -0.8f, 0.9f};
    void advance() {
        static constexpr uint16_t kCycle[4] = {kAm, kF, kC, kG};
        const int mode = static_cast<int>(p(kChords) + 0.5f);
        chord_ = mode == 2 ? (chord_ + 1) & 3 : 0;
        held_ = mode == 0 ? kDrone : mode == 1 ? kAm : kCycle[chord_];
    }

    dsp::SpeechGate gate_;
    float c_[K][P] = {}, s_[K][P] = {}, env_[K] = {};
    float cw_[K][P] = {}, sw_[K][P] = {}, ok_[K][P] = {};
    dsp::OnePole lp_[2][2];
    float a_rms_ = 0, a_att_ = 0, a_rel_ = 0, a_tone_ = 0, ms_ = 0, loud_ = 0;
    int sustain_ = 0, off_for_ = 0, chord_ = -1;
    uint16_t held_ = 0;
    dsp::Ramp r16_, r8_, r4_, rq_, r2_, spread_, level_;
};

FA_NODE(organ, Organ, "Pipe-organ chord (16'..2' ranks) that swells in under a voice and dies away after it. Stereo out, organ only.")

}  // namespace fluent_audio
