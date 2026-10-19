// Core behaviour: compile errors, chunk invariance, RT safety, denormals, NaN, crossfade, digest, latency.
#include <cmath>
#include <cstring>

#include "fa_test.hpp"
#include "fluent_audio/fa.h"

using namespace fluent_audio;
using fa_test::make;
using fa_test::readFile;

static const std::string kDir = std::string(FA_SOURCE_DIR) + "/presets";
static std::string looks() { return readFile(kDir + "/looks.yaml"); }

static bool compileFails(const std::string& yaml, const std::string& needle) {
    Options o;
    o.search_dir = kDir;
    std::string err;
    auto e = Engine::create(yaml, o, err);
    if (e) return false;
    if (err.find(needle) == std::string::npos) {
        std::fprintf(stderr, "  error was: %s\n", err.c_str());
        return false;
    }
    return true;
}

FA_TEST(core, "compile errors carry file:line and say what is wrong") {
    const std::string head = "schema: fluent.sound/v1alpha1\nname: t\n";
    CHECK(compileFails(head + "chain:\n  - fuzz: {}\n", "unknown node 'fuzz'"));
    CHECK(compileFails(head + "chain:\n  - eq: { gain: 3 }\n", "<input>:4: eq: unknown param 'gain'"));
    CHECK(compileFails(head + "chain:\n  - ringmod: { mix: 0.9 }\n", "out of range"));
    CHECK(compileFails(head + "params:\n  x: { type: f32, default: 1 }\n", "range: [min, max] is required"));
    CHECK(compileFails(head + "params:\n  x: { type: f32, default: 2, range: [1, 4] }\nchain:\n  - chorus: { voices: $params.x }\n",
                       "fixed at compile time"));
    CHECK(compileFails(head + "chain:\n  - send: { to: nowhere }\n", "no bus named 'nowhere'"));
    CHECK(compileFails(head + "chain:\n  - limiter: { lookahead_ms: 1.5 }\n", "out of range"));
    CHECK(compileFails(head + "chain:\n  - shimmer: { semitones: 40 }\n", "shimmer.semitones"));
    CHECK(compileFails(head + "extends: nope\n", "extends: 'nope' not found"));
    CHECK(compileFails("name: t\n", "schema: fluent.sound/v1alpha1 is required"));
    CHECK(compileFails(head + "colour: red\n", "unknown key 'colour'"));
}

FA_TEST(core, "every look compiles; aliases resolve; latency 0") {
    auto e = make(looks());
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->presetCount() == 13);
    CHECK(e->presetIndex("system") == e->presetIndex("kepler"));
    CHECK(e->presetIndex("comic") == e->presetIndex("kepler"));
    CHECK(e->presetIndex("liquid") == e->presetIndex("fluent_metal"));
    CHECK(e->latencySamples() == 0);
    CHECK(e->memoryBytes() < 4u * 1024 * 1024) << e->memoryBytes();
    CHECK(e->paramIndex("amount") >= 0);
    CHECK(e->paramIndex("fluent.shimmer1.semitones") >= 0);
    CHECK(e->paramIndex("kepler.eq2.bands.1.gain_db") >= 0);
}

FA_TEST(core, "the dry voice is not delayed (impulse comes out at sample 0)") {
    for (const char* look : {"kepler", "luna", "fluent", "kitt", "hal", "gargantua"}) {
        auto e = make(looks());
        e->select(look);
        std::vector<float> x(256, 0.0f);
        x[0] = 0.5f;
        auto y = fa_test::render(*e, x, 128);
        CHECK(std::fabs(y[0]) > 0.05f) << look << " first sample " << y[0];
    }
}

static std::vector<float> renderWithEvents(int block, uint32_t seed) {
    auto e = make(looks(), 48000, 512);
    const auto x = fa_test::testSignal(48000, 2.0);
    // the same time-stamped events in both runs
    e->selectIdxAt(e->presetIndex("fluent"), 0);
    e->setParamAt(e->paramIndex("amount"), 0.4f, 24000);
    e->selectIdxAt(e->presetIndex("gargantua"), 40000);
    e->setParamAt(e->paramIndex("gargantua.reverb1.decay_s"), 3.0f, 50000);
    e->setParamAt(e->paramIndex("fluent.shimmer1.semitones"), 7.0f, 52000);
    e->selectIdxAt(e->presetIndex("kitt"), 60000);
    e->selectIdxAt(e->presetIndex("hal"), 61000);   // a switch while another is still fading
    std::vector<float> out = fa_test::render(*e, x, block, seed);
    const Analysis& a = e->analysis();
    out.push_back(a.voice);
    out.push_back(a.level);
    out.push_back(a.centroid);
    out.push_back(a.tail);
    return out;
}

FA_TEST(core, "chunk invariance: 128 blocks vs random 1..512 chunks are bit-identical") {
    const auto a = renderWithEvents(128, 0);
    for (uint32_t seed : {7u, 99u, 2024u}) {
        const auto b = renderWithEvents(128, seed);
        CHECK(a.size() == b.size());
        CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0) << "seed " << seed;
    }
}

FA_TEST(core, "process() never allocates (all looks, switching, params)") {
    auto e = make(looks());
    const auto x = fa_test::testSignal(48000, 1.0);
    std::vector<int> idx;
    for (int i = 0; i < e->paramCount(); ++i)
        if (e->param(i).runtime) idx.push_back(i);
    long total = 0;
    size_t pos = 0;
    for (int b = 0; pos + 128 <= x.size(); ++b, pos += 128) {
        if (b % 40 == 0) e->selectIdx((b / 40) % e->presetCount());
        if (b % 7 == 0) e->setParamIdx(idx[(b * 13) % idx.size()], 0.5f * (e->param(idx[(b * 13) % idx.size()]).min + e->param(idx[(b * 13) % idx.size()]).max));
        std::memcpy(e->in(0), &x[pos], 128 * sizeof(float));
        fa_test::NoAlloc guard;
        e->process(128);
        total += guard.count();
    }
    CHECK(total == 0) << total << " allocations";
}

FA_TEST(core, "tails end at exactly zero (denormal flush), no NaN from NaN input") {
    auto e = make(looks());
    e->select("gargantua");
    std::vector<float> x(48000 * 60, 0.0f);
    x[0] = 1.0f;
    x[100] = std::nanf("");
    x[200] = INFINITY;
    auto y = fa_test::render(*e, x, 128);
    bool finite = true;
    for (float v : y) finite &= std::isfinite(v);
    CHECK(finite);
    float last = 0;
    for (size_t i = y.size() - 48000 * 2; i < y.size(); ++i) last = std::max(last, std::fabs(y[i]));
    CHECK(last == 0.0f) << "still " << last << " after 60 s";
    CHECK(e->analysis().tail == 0.0f);
}

FA_TEST(core, "crossfade has no click: the switch adds no sample jump beyond the signal's own") {
    auto e = make(looks());
    std::vector<float> x(48000, 0.0f);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * dsp::sinTurns(static_cast<float>(i) * 200.0f / 48000.0f);
    e->select("kepler");
    std::vector<float> y;
    for (size_t pos = 0; pos < x.size(); pos += 128) {
        if (pos == 24000 - 64) e->select("kitt");
        std::memcpy(e->in(0), &x[pos], 128 * 4);
        e->process(128);
        for (int i = 0; i < 128; ++i) y.push_back(e->out(0)[i]);
    }
    float maxjump = 0;
    for (size_t i = 1; i < y.size(); ++i) maxjump = std::max(maxjump, std::fabs(y[i] - y[i - 1]));
    // a 200 Hz sine at 0.3 moves at most 0.3·2π·200/48000 ≈ 0.008 per sample; EQ/drive may double it
    CHECK(maxjump < 0.03f) << maxjump;
}

FA_TEST(core, "bypass is a fade to the untouched input") {
    auto e = make(looks());
    e->select("fluent");
    std::vector<float> x(48000, 0.0f);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.2f * dsp::sinTurns(static_cast<float>(i) * 440.0f / 48000.0f);
    e->bypass(true);
    std::vector<float> y = fa_test::render(*e, x, 128);
    // after 20 ms fade + tails (fluent's longest is ~3 s, but the input was cut from the start): the dry input + tail
    float err = 0;
    for (size_t i = 2400; i < 4800; ++i) err = std::max(err, std::fabs(y[2 * i] - x[i]));
    CHECK(err < 0.02f) << err;
}

FA_TEST(core, "digest ignores formatting and key order") {
    const std::string a = "schema: fluent.sound/v1alpha1\nname: t\nchain:\n  - eq: { bands: [ { type: hp, f: 80 } ] }\n  - gain: { db: -3 }\n";
    const std::string b = "# same\nname: t\nschema: fluent.sound/v1alpha1\nchain:\n  - eq:   {bands: [{f: 80, type: hp}]}\n  - gain: {db: -3}\n";
    auto e1 = make(a), e2 = make(b);
    CHECK(e1 && e2 && e1->digest() == e2->digest());
    auto e3 = make("schema: fluent.sound/v1alpha1\nname: t\nchain:\n  - gain: { db: -2 }\n");
    CHECK(e3 && e1->digest() != e3->digest());
}

FA_TEST(core, "$params with animate and macro params move the inner nodes") {
    auto e = make(looks());
    e->select("fluent");
    const auto x = fa_test::testSignal(48000, 0.5);
    auto base = fa_test::render(*e, x, 128);
    auto e2 = make(looks());
    e2->select("fluent");
    CHECK(e2->setParam("fluent.shimmer1.semitones", -12));
    CHECK(e2->setParam("amount", 0.0f));
    auto moved = fa_test::render(*e2, x, 128);
    double diff = 0;
    for (size_t i = 0; i < base.size(); ++i) diff += std::fabs(base[i] - moved[i]);
    CHECK(diff > 1.0) << diff;
    CHECK(!e2->setParam("fluent.reverb1.size", 1.0f));   // fixed at compile time
    CHECK(!e2->setParam("nope", 1.0f));
}

FA_TEST(core, "C API round trip") {
    const std::string y = looks();
    FaInstance* bad = fa_create("schema: x\n", 48000, 128);
    CHECK(bad == nullptr);
    CHECK(std::strlen(fa_error()) > 0);
    FaInstance* f = fa_create_ex(y.c_str(), 48000, 128, kDir.c_str());
    CHECK(f != nullptr);
    if (!f) return;
    CHECK(fa_select(f, "comic") == 1);
    CHECK(fa_set_param_f32(f, "amount", 0.7f) == 1);
    CHECK(std::fabs(fa_get_param_f32(f, "amount") - 0.7f) < 1e-6f);
    CHECK(fa_param_index(f, "kepler.reverb1.decay_s") >= 0);
    CHECK(fa_preset_count(f) == 13);
    for (int i = 0; i < 128; ++i) fa_in_buffer(f, 0)[i] = 0.1f;
    CHECK(fa_process(f, 128) == 1);
    CHECK(fa_analysis(f)->version == 1);
    CHECK(fa_latency_samples(f) == 0);
    CHECK(std::strstr(fa_nodes_json(), "\"reverb\"") != nullptr);
    fa_destroy(f);
}
