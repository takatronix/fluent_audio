#include <cstdlib>
#include <cstring>
#include <new>

#include "fa_test.hpp"

extern "C" void* __libc_malloc(size_t);

namespace fa_test {
std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
int g_failures = 0;
bool g_update = false;
std::atomic<long> g_allocs{0};
std::atomic<bool> g_armed{false};

std::vector<float> testSignal(double sr, double seconds) {
    const size_t n = static_cast<size_t>(sr * seconds);
    std::vector<float> x(n, 0.0f);
    fluent_audio::dsp::Rng rng(12345);
    const size_t a = n * 4 / 10, b = n * 6 / 10, c = n * 7 / 10;
    double ph = 0;
    for (size_t i = 0; i < a; ++i) {   // log sweep 40 Hz → 16 kHz at -12 dBFS
        const double t = static_cast<double>(i) / a;
        const double f = 40.0 * fluent_audio::dsp::dexp(t * fluent_audio::dsp::dlog(400.0));
        ph += f / sr;
        x[i] = 0.25f * fluent_audio::dsp::sinTurns(static_cast<float>(ph - static_cast<double>(static_cast<long>(ph))));
    }
    for (size_t i = a; i < b; ++i) x[i] = 0.3f * rng.uni();                 // noise burst
    for (size_t i = b; i < c; ++i) x[i] = 0.4f * fluent_audio::dsp::sinTurns(static_cast<float>((i - b) * 220.0 / sr));
    x[c] = 0.8f;                                                            // an impulse, then silence (tails)
    return x;
}

std::string readFile(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char b[8192];
    size_t k;
    while ((k = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, k);
    std::fclose(f);
    return out;
}

std::unique_ptr<fluent_audio::Engine> make(const std::string& yaml, double sr, int block, const std::string& dir) {
    fluent_audio::Options o;
    o.sample_rate = sr;
    o.max_block = block;
    o.search_dir = dir;
    std::string err;
    auto e = fluent_audio::Engine::create(yaml, o, err);
    if (!e) std::fprintf(stderr, "  compile error: %s\n", err.c_str());
    return e;
}

std::vector<float> render(fluent_audio::Engine& e, const std::vector<float>& in, int block, uint32_t seed) {
    std::vector<float> out(in.size() * 2);
    fluent_audio::dsp::Rng rng(seed ? seed : 1);
    size_t pos = 0;
    while (pos < in.size()) {
        int n = seed ? 1 + static_cast<int>(rng.next() % static_cast<uint32_t>(e.maxBlock())) : block;
        n = static_cast<int>(std::min<size_t>(n, in.size() - pos));
        std::memcpy(e.in(0), &in[pos], sizeof(float) * n);
        std::memcpy(e.in(1), &in[pos], sizeof(float) * n);
        e.process(n);
        for (int i = 0; i < n; ++i) {
            out[2 * (pos + i)] = e.out(0)[i];
            out[2 * (pos + i) + 1] = e.out(1)[i];
        }
        pos += n;
    }
    return out;
}
}  // namespace fa_test

// ---- allocation hooks: count calls while armed ----
void* operator new(size_t n) {
    if (fa_test::g_armed) ++fa_test::g_allocs;
    void* p = __libc_malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
extern "C" void* malloc(size_t n) {
    if (fa_test::g_armed) ++fa_test::g_allocs;
    return __libc_malloc(n);
}

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "";
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--update") fa_test::g_update = true;
    int run = 0;
    for (auto& c : fa_test::cases()) {
        if (!suite.empty() && suite != "all" && suite != c.suite) continue;
        const int before = fa_test::g_failures;
        c.fn();
        ++run;
        std::printf("%s %s/%s\n", fa_test::g_failures == before ? "ok  " : "FAIL", c.suite, c.name);
    }
    std::printf("%d tests, %d failed checks\n", run, fa_test::g_failures);
    return fa_test::g_failures ? 1 : 0;
}
