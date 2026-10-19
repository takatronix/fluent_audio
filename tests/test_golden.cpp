// Golden null tests (design §9): fixed input → render → compare with tests/golden/*.wav.
// Pass: RMS of the difference ≤ -100 dBFS and max ≤ -90 dBFS. `fa_tests golden --update` rewrites the files
// (commit them with a reason). Cases: every registered node at its defaults, every look of presets/looks.yaml
// at 48 kHz and 44.1 kHz, and tests/cases/*.yaml (the scaffold drops one there for each new node).
#include <algorithm>
#include <cmath>
#include <dirent.h>

#include "fa_test.hpp"
#include "wav.hpp"

using namespace fluent_audio;

namespace {
const std::string kGolden = std::string(FA_SOURCE_DIR) + "/tests/golden/";

void golden(const std::string& name, Engine& e, double sr, double seconds) {
    const auto x = fa_test::testSignal(sr, seconds);
    const auto y = fa_test::render(e, x, 128);
    fa_tools::Wav w;
    w.sr = static_cast<int>(sr);
    w.channels = 2;
    w.ch.assign(2, std::vector<float>(x.size()));
    for (size_t i = 0; i < x.size(); ++i) { w.ch[0][i] = y[2 * i]; w.ch[1][i] = y[2 * i + 1]; }
    const std::string path = kGolden + name + ".wav";
    if (fa_test::g_update) {
        fa_tools::writeWav(path, w);
        std::printf("  wrote %s\n", path.c_str());
        return;
    }
    fa_tools::Wav g;
    std::string err;
    if (!fa_tools::readWav(path, g, err)) {
        CHECK(false) << "no golden for " << name << " — run: build/fa_tests golden --update";
        return;
    }
    CHECK(g.frames() == w.frames()) << name;
    if (g.frames() != w.frames()) return;
    double sum = 0, mx = 0;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < w.frames(); ++i) {
            const double d = static_cast<double>(w.ch[c][i]) - g.ch[c][i];
            sum += d * d;
            mx = std::max(mx, std::fabs(d));
        }
    const double rms_db = 10 * std::log10(sum / (2.0 * w.frames()) + 1e-30), max_db = 20 * std::log10(mx + 1e-30);
    CHECK(rms_db <= -100 && max_db <= -90) << name << ": diff rms " << rms_db << " dBFS, max " << max_db << " dBFS";
}
}  // namespace

FA_TEST(golden, "nodes at their defaults") {
    for (const NodeInfo* n : nodeRegistry()) {
        std::string y = "schema: fluent.sound/v1alpha1\nname: g\n";
        const std::string nm = n->name;
        if (nm == "send") y += "chain:\n  - send: { to: b, gain: 0.5 }\nbuses:\n  b:\n    - reverb: { decay_s: 0.4 }\n";
        else if (nm == "mix") y += "chain:\n  - exciter: { mix: 0.3 }\nmaster:\n  - mix: { amount: 0.5 }\n";
        else y += "chain:\n  - " + nm + ": {}\n";
        auto e = fa_test::make(y);
        CHECK(e != nullptr) << nm;
        if (e) golden("node_" + nm, *e, 48000, 0.35);
    }
}

FA_TEST(golden, "looks at 48 kHz and 44.1 kHz") {
    const std::string bank = fa_test::readFile(std::string(FA_SOURCE_DIR) + "/presets/looks.yaml");
    for (double sr : {48000.0, 44100.0}) {
        auto probe = fa_test::make(bank, sr);
        if (!probe) { CHECK(false); return; }
        for (int p = 0; p < probe->presetCount(); ++p) {
            auto e = fa_test::make(bank, sr);
            e->selectIdx(p);
            golden("look_" + probe->presetName(p) + (sr == 48000.0 ? "_48k" : "_44k"), *e, sr, 0.6);
        }
    }
}

FA_TEST(golden, "tests/cases/*.yaml") {
    const std::string dir = std::string(FA_SOURCE_DIR) + "/tests/cases/";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    while (dirent* ent = readdir(d)) {
        const std::string f = ent->d_name;
        if (f.size() > 5 && f.compare(f.size() - 5, 5, ".yaml") == 0) files.push_back(f);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        auto e = fa_test::make(fa_test::readFile(dir + f), 48000, 128, dir);
        CHECK(e != nullptr) << f;
        if (e) golden("case_" + f.substr(0, f.size() - 5), *e, 48000, 0.5);
    }
}
