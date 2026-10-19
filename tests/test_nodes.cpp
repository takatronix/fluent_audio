// Generic tests every registered node gets for free (a new node in src/nodes/ is covered without writing any):
// compiles at defaults and at its range ends, output stays finite, no allocation in process, chunk invariance,
// and the tail ends at exactly 0 after the input stops.
#include <cmath>
#include <cstring>

#include "fa_test.hpp"

using namespace fluent_audio;

namespace {

// A Sound with one node of this type, params at defaults ("def"), all mins ("min") or all maxes ("max").
std::string soundFor(const NodeInfo& n, const std::string& mode) {
    auto val = [&](const ParamSpec& p) -> std::string {
        float v = mode == "min" ? p.min : mode == "max" ? p.max : p.def;
        char b[32];
        std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
        return b;
    };
    std::string args, group;
    for (int i = 0; i < n.nparams; ++i) {
        const ParamSpec& p = n.params[i];
        if (p.group) {
            group += (group.empty() ? "" : ", ") + std::string(p.name) + ": " + val(p);
            continue;
        }
        args += (args.empty() ? "" : ", ") + std::string(p.name) + ": " + val(p);
    }
    for (int i = 0; i < n.nparams; ++i)
        if (n.params[i].group) {
            args += (args.empty() ? "" : ", ") + std::string(n.params[i].group) + ": [ { " + group + " }, { " + group + " } ]";
            break;
        }
    const std::string name = n.name;
    std::string s = "schema: fluent.sound/v1alpha1\nname: t_" + name + "\n";
    if (name == "send") return s + "chain:\n  - send: { to: b, " + args + " }\nbuses:\n  b:\n    - gain: { db: 0 }\n";
    if (name == "mix") return s + "chain:\n  - gain: { db: -6 }\nmaster:\n  - mix: { " + args + " }\n";
    return s + "chain:\n  - " + name + ": { " + args + " }\n";
}

}  // namespace

FA_TEST(nodes, "every node: defaults and range ends compile, stay finite, never allocate") {
    const auto x = fa_test::testSignal(48000, 1.0);
    for (const NodeInfo* n : nodeRegistry()) {
        for (const char* mode : {"def", "min", "max"}) {
            auto e = fa_test::make(soundFor(*n, mode));
            CHECK(e != nullptr) << n->name << " " << mode;
            if (!e) continue;
            bool finite = true;
            long allocs = 0;
            float peak = 0;
            for (size_t pos = 0; pos + 128 <= x.size(); pos += 128) {
                std::memcpy(e->in(0), &x[pos], 128 * 4);
                fa_test::NoAlloc guard;
                e->process(128);
                allocs += guard.count();
                for (int i = 0; i < 128; ++i) {
                    finite &= std::isfinite(e->out(0)[i]) && std::isfinite(e->out(1)[i]);
                    peak = std::max(peak, std::fabs(e->out(0)[i]));
                }
            }
            CHECK(finite) << n->name << " " << mode;
            CHECK(allocs == 0) << n->name << " " << mode << ": " << allocs << " allocations in process";
            CHECK(peak < 64.0f) << n->name << " " << mode << " peak " << peak;
            CHECK(e->diagnosticsCount() == 0) << n->name << " " << mode << " NaN resets";
        }
    }
}

FA_TEST(nodes, "every node: random chunking gives the same bits") {
    const auto x = fa_test::testSignal(48000, 0.5);
    for (const NodeInfo* n : nodeRegistry()) {
        auto a = fa_test::make(soundFor(*n, "def"), 48000, 512);
        auto b = fa_test::make(soundFor(*n, "def"), 48000, 512);
        if (!a || !b) { CHECK(false) << n->name; continue; }
        const auto ya = fa_test::render(*a, x, 128), yb = fa_test::render(*b, x, 0, 4242);
        CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * 4) == 0) << n->name;
    }
}

FA_TEST(nodes, "every node: silence after the input stops ends at exactly 0") {
    for (const NodeInfo* n : nodeRegistry()) {
        auto e = fa_test::make(soundFor(*n, "def"));
        if (!e) { CHECK(false) << n->name; continue; }
        std::vector<float> x(48000 * 40, 0.0f);
        for (int i = 0; i < 4800; ++i) x[i] = 0.5f * dsp::sinTurns(i * 300.0f / 48000.0f);
        const auto y = fa_test::render(*e, x, 128);
        float tail = 0;
        for (size_t i = y.size() - 9600; i < y.size(); ++i) tail = std::max(tail, std::fabs(y[i]));
        CHECK(tail == 0.0f) << n->name << " still " << tail;
    }
}

FA_TEST(nodes, "reverb: RT60 follows decay_s; loop stays stable at the extremes") {
    for (double t60 : {0.5, 2.0, 6.0}) {
        char y[256];
        std::snprintf(y, sizeof y, "schema: fluent.sound/v1alpha1\nname: r\nchain:\n  - reverb: { decay_s: %g, damp_hz: 20000, mod: 0 }\n", t60);
        auto e = fa_test::make(y);
        std::vector<float> x(static_cast<size_t>(48000 * (t60 * 1.5 + 0.5)), 0.0f);
        for (int i = 0; i < 4800; ++i) x[i] = 0.5f * dsp::sinTurns(i * 400.0f / 48000.0f);   // a low tone burst (damping only shortens highs)
        const auto out = fa_test::render(*e, x, 128);
        auto energy = [&](double t0, double t1) {
            double s = 0;
            for (size_t i = static_cast<size_t>(t0 * 48000); i < static_cast<size_t>(t1 * 48000); ++i) s += out[2 * i] * out[2 * i];
            return s;
        };
        // decay over [0.2, 0.2 + t60/2] should be about 30 dB
        const double w = 0.05, a = 0.2, b = 0.2 + t60 / 2;
        const double drop = 10 * std::log10(energy(a, a + w) / energy(b, b + w));
        CHECK(drop > 22 && drop < 38) << "t60 " << t60 << " drop over t60/2 = " << drop << " dB";
    }
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: r\nchain:\n  - reverb: { decay_s: 20, size: 2.5, damp_hz: 20000, mod: 1, diffusion: 0.9 }\n");
    std::vector<float> x(48000 * 20, 0.0f);
    x[0] = 1;
    const auto out = fa_test::render(*e, x, 128);
    float late = 0;
    for (size_t i = out.size() - 48000; i < out.size(); ++i) late = std::max(late, std::fabs(out[i]));
    CHECK(late < 0.5f) << late;
}

FA_TEST(nodes, "limiter: output never exceeds the ceiling") {
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: l\nchain:\n  - gain: { db: 24 }\n  - limiter: { ceiling_db: -1 }\n");
    const auto x = fa_test::testSignal(48000, 1.0);
    const auto y = fa_test::render(*e, x, 128);
    float pk = 0;
    for (float v : y) pk = std::max(pk, std::fabs(v));
    CHECK(pk <= static_cast<float>(dsp::db2lin(-1.0)) + 1e-6f) << pk;
}

FA_TEST(nodes, "eq: a 1 kHz +6 dB peak gives +6 dB at 1 kHz") {
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - eq: { bands: [ { type: peak, f: 1000, q: 1, gain_db: 6 } ] }\n");
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.1f * dsp::sinTurns(i * 1000.0f / 48000.0f);
    const auto y = fa_test::render(*e, x, 128);
    double si = 0, so = 0;
    for (size_t i = 24000; i < 48000; ++i) { si += x[i] * x[i]; so += y[2 * i] * y[2 * i]; }
    const double g = 10 * std::log10(so / si);
    CHECK(std::fabs(g - 6.0) < 0.1) << g;
}

FA_TEST(nodes, "microshift: mix 0 is the dry, undelayed; the left voice is detuned up by `cents`") {
    std::vector<float> x(96000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.1f * dsp::sinTurns(static_cast<float>(i % 48) / 48.0f);   // 1 kHz
    auto d = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - microshift: { mix: 0 }\n");
    const auto y0 = fa_test::render(*d, x, 128);
    bool same = true;
    for (size_t i = 0; i < x.size(); ++i) same = same && y0[2 * i] == x[i] && y0[2 * i + 1] == x[i];
    CHECK(same);
    // wet only on the left: subtract the dry, count zero crossings of what is left
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - microshift: { cents: 50, mix: 1, spread: 1 }\n");
    const auto y = fa_test::render(*e, x, 128);
    int zc = 0;
    float prev = 0;
    for (size_t i = 48000; i < 96000; ++i) {
        const float w = y[2 * i] - x[i];
        if (prev <= 0 && w > 0) ++zc;
        prev = w;
    }
    CHECK(std::abs(zc - 1029) <= 3) << zc;   // 1000 Hz · 2^(50/1200)
}

FA_TEST(nodes, "phaser: depth 0 is the dry; depth 1 with a parked sweep cuts a notch") {
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.1f * dsp::sinTurns(static_cast<float>(i % 48) / 48.0f);   // 1 kHz
    auto d = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - phaser: { depth: 0 }\n");
    const auto y0 = fa_test::render(*d, x, 128);
    bool same = true;
    for (size_t i = 0; i < x.size(); ++i) same = same && y0[2 * i] == x[i] && y0[2 * i + 1] == x[i];
    CHECK(same);
    // 2 stages at a fixed corner f: the allpass is -180° at f, so (x + ap)/2 nulls there
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n"
                           "  - phaser: { stages: 2, low_hz: 1000, high_hz: 1000, feedback: 0, depth: 1 }\n");
    const auto y = fa_test::render(*e, x, 128);
    double si = 0, so = 0;
    for (size_t i = 24000; i < 48000; ++i) { si += x[i] * x[i]; so += y[2 * i] * y[2 * i]; }
    CHECK(10 * std::log10(so / si) < -30) << 10 * std::log10(so / si);
}

namespace {
// Energy of y (interleaved stereo, left) at `hz` over [a, b) seconds — a single-bin DFT.
double binDb(const std::vector<float>& y, double hz, double a, double b, int ch = 0) {
    double re = 0, im = 0;
    const size_t i0 = static_cast<size_t>(a * 48000), i1 = static_cast<size_t>(b * 48000);
    for (size_t i = i0; i < i1; ++i) {
        const double ph = 2 * 3.14159265358979 * hz * static_cast<double>(i) / 48000;
        re += y[2 * i + ch] * std::cos(ph);
        im += y[2 * i + ch] * std::sin(ph);
    }
    return 10 * std::log10((re * re + im * im) / static_cast<double>((i1 - i0) * (i1 - i0)) + 1e-30);
}
// Tone bursts of `hz` at [start, end) seconds.
std::vector<float> bursts(double seconds, double hz, std::initializer_list<std::pair<double, double>> spans) {
    std::vector<float> x(static_cast<size_t>(seconds * 48000), 0.0f);
    for (auto [a, b] : spans)
        for (size_t i = static_cast<size_t>(a * 48000); i < static_cast<size_t>(b * 48000); ++i)
            x[i] = 0.3f * dsp::sinTurns(static_cast<float>(hz * static_cast<double>(i) / 48000.0));
    return x;
}
}  // namespace

FA_TEST(nodes, "organ: silent until a voice; plays A2/A1 under it; cycle moves to F after a pause; dies after") {
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n"
                           "  - organ: { chords: cycle, release_s: 1, sustain_s: 0.2, quint: 0, r2: 0 }\n");
    const auto x = bursts(8, 1000, {{0.5, 1.5}, {2.0, 3.0}});
    const auto y = fa_test::render(*e, x, 128);
    float before = 0;
    for (size_t i = 0; i < 24000; ++i) before = std::max(before, std::fabs(y[2 * i]));
    CHECK(before == 0.0f) << before;
    const double a2 = binDb(y, 110, 1.0, 1.5), f2 = binDb(y, 87.307, 1.0, 1.5);
    CHECK(a2 > -50 && a2 - f2 > 20) << a2 << " " << f2;                         // Am: A2, no F2
    const double f2b = binDb(y, 87.307, 2.5, 3.0), a1b = binDb(y, 55, 2.5, 3.0);
    CHECK(f2b > -50 && f2b - a1b > 15) << f2b << " " << a1b;                    // F: F2 rings, the 16' A1 has gone
    float late = 0;
    for (size_t i = 7 * 48000; i < 8 * 48000; ++i) late = std::max(late, std::fabs(y[2 * i]));
    CHECK(late == 0.0f) << late;
}

FA_TEST(nodes, "choir: mix 0 is the dry, undelayed; the copies are spread (L ≠ R)") {
    const auto x = fa_test::testSignal(48000, 1.0);
    auto d = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - choir: { mix: 0 }\n");
    const auto y0 = fa_test::render(*d, x, 128);
    bool same = true;
    for (size_t i = 0; i < x.size(); ++i) same = same && y0[2 * i] == x[i] && y0[2 * i + 1] == x[i];
    CHECK(same);
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - choir: { mix: 1, dry: 0 }\n");
    const auto y = fa_test::render(*e, x, 128);
    double m = 0, s = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        m += (y[2 * i] + y[2 * i + 1]) * (y[2 * i] + y[2 * i + 1]);
        s += (y[2 * i] - y[2 * i + 1]) * (y[2 * i] - y[2 * i + 1]);
    }
    CHECK(m > 0 && s / m > 0.05) << s / m;
}

FA_TEST(nodes, "subharmonic: a 200 Hz tone gets a 100 Hz partner; amount 0 is the dry") {
    const auto x = bursts(1, 200, {{0, 1}});
    auto d = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - subharmonic: { amount: 0 }\n");
    const auto y0 = fa_test::render(*d, x, 128);
    bool same = true;
    for (size_t i = 0; i < x.size(); ++i) same = same && y0[2 * i] == x[i];
    CHECK(same);
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n  - subharmonic: { amount: 1, lp_hz: 140 }\n");
    const auto y = fa_test::render(*e, x, 128);
    const double sub = binDb(y, 100, 0.3, 1.0), tone = binDb(y, 200, 0.3, 1.0), q = binDb(y, 50, 0.3, 1.0);
    CHECK(sub > tone - 20 && sub - q > 20) << sub << " " << tone << " " << q;
}

FA_TEST(nodes, "dilate: silent while the voice sounds; after it, the last stretch hangs, sinking; then 0") {
    auto e = fa_test::make("schema: fluent.sound/v1alpha1\nname: q\nchain:\n"
                           "  - dilate: { decay_s: 2, glide_st: -12, level: 1 }\n");
    const auto x = bursts(8, 440, {{0.2, 1.2}});
    const auto y = fa_test::render(*e, x, 128);
    float during = 0;
    for (size_t i = 0; i < static_cast<size_t>(1.2 * 48000); ++i) during = std::max(during, std::fabs(y[2 * i]));
    CHECK(during == 0.0f) << during;
    auto zc = [&](double a, double b) {   // zero crossings per second
        int n = 0;
        for (size_t i = static_cast<size_t>(a * 48000); i < static_cast<size_t>(b * 48000); ++i) n += (y[2 * i - 2] <= 0) != (y[2 * i] <= 0);
        return n / 2.0 / (b - a);
    };
    double ms = 0;
    for (size_t i = static_cast<size_t>(1.7 * 48000); i < static_cast<size_t>(1.9 * 48000); ++i) ms += y[2 * i] * y[2 * i];
    CHECK(std::sqrt(ms / 9600) > 0.03) << std::sqrt(ms / 9600);               // the tone hangs on
    const double z0 = zc(1.7, 1.9), z1 = zc(2.7, 2.9);
    CHECK(z0 > 380 && z0 < 445 && z1 < 0.8 * z0) << z0 << " " << z1;           // and sinks
    float end = 0;
    for (size_t i = 7 * 48000; i < 8 * 48000; ++i) end = std::max(end, std::fabs(y[2 * i]));
    CHECK(end == 0.0f) << end;
}
