#pragma once
// Small DSP toolkit shared by every node: own math approximations (design D8 — no libm in the signal path, so
// native and wasm give the same bits), biquads, delay lines, one-poles, a deterministic RNG, denormal flush.
//
// Rules this file keeps (and nodes should too): float32 for audio, double only for coefficients; only + - * /,
// floor, ldexp/frexp and sqrt (all exact in IEEE) — never std::sin / std::exp / std::tanh in the signal path.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace fluent_audio {
namespace dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;
constexpr double kLn10 = 2.30258509299404568402;

// ---------------------------------------------------------------- double (coefficients, control rate)
inline double dexp(double x) {
    if (x < -745.0) return 0.0;
    if (x > 709.0) x = 709.0;
    const double k = std::floor(x * 1.44269504088896340736 + 0.5);
    const double r = (x - k * 6.93147180369123816490e-01) - k * 1.90821492927058770002e-10;   // Cody-Waite
    double p = 1.0 / 479001600.0;                                                             // 1/12!
    const double inv[] = {39916800.0, 3628800.0, 362880.0, 40320.0, 5040.0, 720.0, 120.0, 24.0, 6.0, 2.0, 1.0, 1.0};
    for (double d : inv) p = p * r + 1.0 / d;
    return std::ldexp(p, static_cast<int>(k));
}

inline double dlog(double x) {
    if (!(x > 0.0)) return -745.0;
    int e = 0;
    double m = std::frexp(x, &e);               // m in [0.5, 1)
    if (m < 0.70710678118654752440) { m *= 2.0; e -= 1; }
    const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    double p = 1.0 / 21.0;
    for (int k = 19; k >= 1; k -= 2) p = p * s2 + 1.0 / k;
    return e * kLn2 + 2.0 * s * p;
}

inline double dsin_core(double r) {   // |r| <= pi/4, Taylor to r^17
    const double r2 = r * r;
    double p = 1.0 / 355687428096000.0;
    p = p * r2 - 1.0 / 1307674368000.0;
    p = p * r2 + 1.0 / 6227020800.0;
    p = p * r2 - 1.0 / 39916800.0;
    p = p * r2 + 1.0 / 362880.0;
    p = p * r2 - 1.0 / 5040.0;
    p = p * r2 + 1.0 / 120.0;
    p = p * r2 - 1.0 / 6.0;
    p = p * r2 + 1.0;
    return r * p;
}
inline double dcos_core(double r) {   // |r| <= pi/4, Taylor to r^18
    const double r2 = r * r;
    double p = -1.0 / 6402373705728000.0;
    p = p * r2 + 1.0 / 20922789888000.0;
    p = p * r2 - 1.0 / 87178291200.0;
    p = p * r2 + 1.0 / 479001600.0;
    p = p * r2 - 1.0 / 3628800.0;
    p = p * r2 + 1.0 / 40320.0;
    p = p * r2 - 1.0 / 720.0;
    p = p * r2 + 1.0 / 24.0;
    p = p * r2 - 1.0 / 2.0;
    p = p * r2 + 1.0;
    return p;
}
inline void dsincos(double x, double& s, double& c) {
    const double k = std::floor(x * 0.63661977236758134308 + 0.5);            // x / (pi/2)
    const double r = (x - k * 1.57079632673412561417) - k * 6.07710050650619224932e-11;
    const double sr = dsin_core(r), cr = dcos_core(r);
    const int q = static_cast<int>(static_cast<int64_t>(k) & 3);
    switch (q) {
        case 0: s = sr; c = cr; break;
        case 1: s = cr; c = -sr; break;
        case 2: s = -sr; c = -cr; break;
        default: s = -cr; c = sr; break;
    }
}
inline double dsin(double x) { double s, c; dsincos(x, s, c); return s; }
inline double dcos(double x) { double s, c; dsincos(x, s, c); return c; }
inline double dpow(double b, double e) { return dexp(e * dlog(b)); }
inline double db2lin(double db) { return dexp(db * (kLn10 / 20.0)); }
inline double lin2db(double g) { return dlog(g) * (20.0 / kLn10); }
// One-pole smoothing coefficient for a time constant in ms (y += (1-a)(x-y)).
inline double poleMs(double ms, double sr) { return ms <= 0.0 ? 0.0 : dexp(-1000.0 / (ms * sr)); }
inline double poleHz(double hz, double sr) { return dexp(-2.0 * kPi * hz / sr); }

// ---------------------------------------------------------------- float (per sample)
// Values below 2^-100 in a feedback path are set to 0 (design §3.5): wasm cannot set FTZ, so this is the one
// place where a decaying tail ends, identically everywhere.
inline float flush(float x) { return (x > -7.88860905e-31f && x < 7.88860905e-31f) ? 0.0f : x; }
inline bool finite(float x) { return x - x == 0.0f; }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// sin(2*pi*t) for any t (turns). |error| < 1e-7.
inline float sinTurns(float t) {
    t = t - std::floor(t + 0.5f);                      // [-0.5, 0.5)
    float x = t * 6.28318530717958647f;               // [-pi, pi)
    if (x > 1.57079632679f) x = 3.14159265359f - x;
    else if (x < -1.57079632679f) x = -3.14159265359f - x;
    const float x2 = x * x;
    float p = -2.50521084e-8f;                         // -1/11!
    p = p * x2 + 2.75573192e-6f;
    p = p * x2 - 1.98412698e-4f;
    p = p * x2 + 8.33333333e-3f;
    p = p * x2 - 1.66666667e-1f;
    p = p * x2 + 1.0f;
    return x * p;
}

// e^x in float. Relative error ~2e-7.
inline float fexp(float x) {
    if (x < -87.0f) return 0.0f;
    if (x > 87.0f) x = 87.0f;
    const float t = x * 1.44269504089f;
    const float k = std::floor(t);
    const float f = (t - k) * 0.693147180560f;        // in [0, ln2)
    float p = 1.0f / 5040.0f;
    p = p * f + 1.0f / 720.0f;
    p = p * f + 1.0f / 120.0f;
    p = p * f + 1.0f / 24.0f;
    p = p * f + 1.0f / 6.0f;
    p = p * f + 0.5f;
    p = p * f + 1.0f;
    p = p * f + 1.0f;
    const int32_t bits = (static_cast<int32_t>(k) + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

inline float ftanh(float x) {
    const float ax = x < 0.0f ? -x : x;
    if (ax < 0.02f) return x - x * x * x * (1.0f / 3.0f);
    if (ax > 9.0f) return x < 0.0f ? -1.0f : 1.0f;
    const float e = fexp(2.0f * ax);
    const float t = 1.0f - 2.0f / (e + 1.0f);
    return x < 0.0f ? -t : t;
}

// log(cosh(x)) in double: the antiderivative of tanh, for first-order ADAA.
inline double logcosh(double x) {
    const double ax = x < 0.0 ? -x : x;
    return ax + dlog(1.0 + dexp(-2.0 * ax)) - kLn2;
}

// First-order antiderivative-antialiased tanh (design §4 saturate). Adds half a sample of delay.
struct AdaaTanh {
    double x1 = 0.0, f1 = 0.0;
    void reset() { x1 = 0.0; f1 = 0.0; }
    float operator()(float xf) {
        const double x = xf;
        const double f = logcosh(x);
        const double dx = x - x1;
        float y;
        if (dx > 1e-4 || dx < -1e-4) y = static_cast<float>((f - f1) / dx);
        else y = ftanh(static_cast<float>(0.5 * (x + x1)));
        x1 = x;
        f1 = f;
        return y;
    }
};

// A value that moves once per control tick (32 samples) and is interpolated per sample in between (gains: no
// zipper noise). Call next() in control(), at() in process() with the sample's position within the tick.
struct Ramp {
    float a = 1, b = 1;
    bool fresh = true;
    void next(float v) { a = fresh ? v : b; b = v; fresh = false; }
    void reset() { fresh = true; }
    float at(int pos) const { return a + (b - a) * (static_cast<float>(pos + 1) * (1.0f / 32.0f)); }
};

// ---------------------------------------------------------------- RNG (xorshift32, fixed seed)
struct Rng {
    uint32_t s = 0x9E3779B9u;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f; }   // [-1, 1)
};

// ---------------------------------------------------------------- biquad (RBJ cookbook, TDF-II transposed)
enum class BiquadType { kHp, kLp, kBp, kPeak, kLowShelf, kHighShelf, kNotch, kAllpass };
constexpr const char* kBiquadChoices = "hp|lp|bp|peak|lowshelf|highshelf|notch|allpass";

struct BiquadCoef {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

inline BiquadCoef rbj(BiquadType type, double f, double q, double gain_db, double sr) {
    const double nyq = 0.49 * sr;
    if (f > nyq) f = nyq;
    if (f < 1.0) f = 1.0;
    if (q < 0.05) q = 0.05;
    const double A = db2lin(gain_db * 0.5);           // 10^(g/40)
    double sw, cw;
    dsincos(2.0 * kPi * f / sr, sw, cw);
    const double al = sw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
        case BiquadType::kHp: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BiquadType::kLp: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BiquadType::kBp: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BiquadType::kNotch: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BiquadType::kAllpass: b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case BiquadType::kPeak:
            b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
        case BiquadType::kLowShelf: {
            const double sq = 2 * std::sqrt(A) * al;
            b0 = A * ((A + 1) - (A - 1) * cw + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sq);
            a0 = (A + 1) + (A - 1) * cw + sq; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sq;
            break;
        }
        default: {   // high shelf
            const double sq = 2 * std::sqrt(A) * al;
            b0 = A * ((A + 1) + (A - 1) * cw + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sq);
            a0 = (A + 1) - (A - 1) * cw + sq; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sq;
            break;
        }
    }
    BiquadCoef c;
    c.b0 = static_cast<float>(b0 / a0); c.b1 = static_cast<float>(b1 / a0); c.b2 = static_cast<float>(b2 / a0);
    c.a1 = static_cast<float>(a1 / a0); c.a2 = static_cast<float>(a2 / a0);
    return c;
}

struct BiquadState {
    float s1 = 0, s2 = 0;
    void reset() { s1 = s2 = 0; }
    float run(const BiquadCoef& c, float x) {
        const float y = c.b0 * x + s1;
        s1 = flush(c.b1 * x - c.a1 * y + s2);
        s2 = flush(c.b2 * x - c.a2 * y);
        return y;
    }
};

// One-pole low-pass, DC gain 1: y += (1-a)(x-y).
struct OnePole {
    float y = 0;
    void reset() { y = 0; }
    float run(float a, float x) { y = flush(x + a * (y - x)); return y; }
};

// ---------------------------------------------------------------- delay line (power-of-two ring)
struct DelayLine {
    float* buf = nullptr;
    uint32_t mask = 0, w = 0;
    static uint32_t sizeFor(uint32_t max_delay) { uint32_t n = 1; while (n < max_delay + 4) n <<= 1; return n; }
    void attach(float* mem, uint32_t size) { buf = mem; mask = size - 1; w = 0; }
    void reset() { if (buf) std::memset(buf, 0, sizeof(float) * (mask + 1)); w = 0; }
    void push(float x) { buf[w] = flush(x); w = (w + 1) & mask; }
    // delay d >= 1: d = 1 is the sample pushed last.
    float tap(uint32_t d) const { return buf[(w - d) & mask]; }
    float linear(float d) const {
        const float fl = std::floor(d);
        const uint32_t i = static_cast<uint32_t>(fl);
        const float f = d - fl;
        const float a = tap(i), b = tap(i + 1);
        return a + (b - a) * f;
    }
    float hermite(float d) const {   // d >= 2
        const float fl = std::floor(d);
        const uint32_t i = static_cast<uint32_t>(fl);
        const float f = d - fl;
        const float xm1 = tap(i - 1), x0 = tap(i), x1 = tap(i + 1), x2 = tap(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
};

}  // namespace dsp
}  // namespace fluent_audio
