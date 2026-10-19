#pragma once
// A very small test runner: FA_TEST(suite, name) { ... CHECK(cond) << "msg"; }
#include <atomic>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "fluent_audio/engine.hpp"

namespace fa_test {

struct Case {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};
std::vector<Case>& cases();
struct Reg {
    Reg(const char* s, const char* n, std::function<void()> f) { cases().push_back({s, n, std::move(f)}); }
};
extern int g_failures;
extern bool g_update;   // golden --update

struct Check {
    bool ok;
    const char* file;
    int line;
    const char* expr;
    std::ostringstream msg;
    Check(bool o, const char* f, int l, const char* e) : ok(o), file(f), line(l), expr(e) {}
    ~Check() {
        if (!ok) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL %s:%d: %s %s\n", file, line, expr, msg.str().c_str());
        }
    }
    template <class T>
    Check& operator<<(const T& v) { if (!ok) msg << v; return *this; }
};

// Allocation counter (operator new / malloc hooks in test_main.cpp); counts only while armed.
extern std::atomic<long> g_allocs;
extern std::atomic<bool> g_armed;
struct NoAlloc {
    NoAlloc() { g_allocs = 0; g_armed = true; }
    ~NoAlloc() { g_armed = false; }
    long count() const { return g_allocs.load(); }
};

// Deterministic test signal: a log sweep, a noise burst, a short tone and silence.
std::vector<float> testSignal(double sr, double seconds);
std::string readFile(const std::string& path);
std::unique_ptr<fluent_audio::Engine> make(const std::string& yaml, double sr = 48000, int block = 128,
                                           const std::string& dir = std::string(FA_SOURCE_DIR) + "/presets");
// Render mono input through the engine in fixed blocks (or random 1..max_block chunks if seed != 0).
std::vector<float> render(fluent_audio::Engine& e, const std::vector<float>& in, int block, uint32_t seed = 0);

}  // namespace fa_test

#define FA_CAT2(a, b) a##b
#define FA_CAT(a, b) FA_CAT2(a, b)
#define FA_TEST(suite, name)                                                             \
    static void FA_CAT(fa_t_, __LINE__)();                                               \
    static fa_test::Reg FA_CAT(fa_r_, __LINE__)(#suite, name, &FA_CAT(fa_t_, __LINE__)); \
    static void FA_CAT(fa_t_, __LINE__)()
#define CHECK(cond) fa_test::Check(static_cast<bool>(cond), __FILE__, __LINE__, #cond)
