#pragma once
// The runtime: a bank of compiled presets (a single Sound is a bank of one), switched with a crossfade, a bank
// master (the one safety limiter), $params with animate curves, time-stamped events, and the analysis values.
//
//   std::string err;
//   auto eng = fluent_audio::Engine::create(yaml_text, {48000, 128, "presets/"}, err);
//   eng->select("fluent");                         // crossfade (300 ms), old tails ring out
//   eng->setParam("amount", 0.5f);                 // animate curve of the $param
//   memcpy(eng->in(0), voice, n * 4); eng->process(n); read eng->out(0), eng->out(1)
//
// process() is real-time safe: no allocation, no locks, no syscalls. The setters may be called from one other
// thread (they only push into a fixed SPSC queue); everything is applied at control ticks (absolute frame % 32),
// so the same time-stamped events give bit-identical output however the host chunks its calls.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fluent_audio/node.hpp"

namespace fluent_audio {

struct Options {
    double sample_rate = 48000;
    int max_block = 128;
    std::string search_dir;   // where `extends:`, bank presets and macros are looked up (besides the pack)
};

// Design §6. Fixed layout (the wasm side reads it straight from memory).
struct Analysis {
    uint32_t version = 1;
    uint32_t size = sizeof(Analysis);
    uint64_t frame = 0;      // last sample these values include
    float voice = 0;         // the voice before colouring (dry tap), 0..1 (-60..-6 dBFS) — the mouth
    float level = 0;         // the output (with tails), same scale — glow
    float peak = 0;          // output sample peak, decaying (~300 ms)
    float onset = 0;         // 1 at a syllable onset, decays in ~50 ms
    float centroid = 0;      // brightness, 0..1 (300 Hz .. 5 kHz, log)
    float tail = 0;          // 1 while our own sound is still audible (output > -60 dBFS, 100 ms hold)
};

struct GraphParam {
    std::string name;
    float min = 0, max = 1, def = 0;
    float cur = 0, from = 0, to = 0;
    double t = 0, dur = 0.02;   // seconds
    int ease_kind = 0;          // 0 linear, 1 in, 2 out, 3 in_out
    bool moving = false, moved = true;
    void set(float v);
    void tick(double dt);
};

struct Binding {
    GraphParam* gp;
    Node* node;
    int slot;
    double k, c;
};

class Preset;

struct ParamHandle {
    std::string name;
    float value = 0, min = 0, max = 0;
    bool runtime = true;
    struct Target {
        GraphParam* gp = nullptr;
        Node* node = nullptr;
        int slot = 0;
        double k = 1, c = 0;
    };
    std::vector<Target> targets;
};

class Engine {
public:
    static std::unique_ptr<Engine> create(const std::string& yaml, const Options& opt, std::string& error);
    ~Engine();

    // ---- audio (RT) ----
    float* in(int ch) { return in_[ch & 1]; }
    float* out(int ch) { return out_[ch & 1]; }
    int process(int frames);                       // in → out, frames <= max_block. 1 = ok
    void processSpan(const float* const* in, int in_channels, float* outL, float* outR, int frames);

    // ---- control (any one thread; queued, applied at the next control tick or at `frame`) ----
    int paramIndex(const std::string& name) const;
    int paramCount() const { return static_cast<int>(handles_.size()); }
    const ParamHandle& param(int i) const { return handles_[i]; }
    bool setParam(const std::string& name, float v);
    bool setParamIdx(int idx, float v);
    bool setParamAt(int idx, float v, uint64_t frame);
    float getParam(const std::string& name) const;
    int presetIndex(const std::string& name) const;   // aliases resolve here
    int presetCount() const;
    const std::string& presetName(int i) const;
    int aliasCount() const { return static_cast<int>(aliases_.size()); }
    const std::pair<std::string, int>& alias(int i) const { return aliases_[i]; }
    bool select(const std::string& name);
    bool selectIdx(int idx);
    bool selectIdxAt(int idx, uint64_t frame);
    bool bypass(bool on);
    void reset();                                       // drop all state (tails too); call when silent
    int current() const { return active_; }

    // ---- read ----
    const Analysis& analysis() const { return analysis_; }
    int diagnosticsCount() const { return diag_count_.load(); }
    std::string takeDiagnostics();
    const std::string& digest() const { return digest_; }
    int latencySamples() const { return 0; }
    uint64_t frame() const { return frame_; }
    size_t memoryBytes() const;
    double sampleRate() const { return sr_; }
    int maxBlock() const { return max_block_; }
    int inChannels() const { return in_channels_; }
    std::string describe() const;                       // compiled structure, for humans / tests

    // ---- built by the compiler ----
    struct Event {
        uint64_t frame;      // 0 = at the next tick
        int32_t kind;        // 0 param, 1 select, 2 bypass, 3 reset
        int32_t idx;
        float value;
    };

private:
    friend class Compiler;
    Engine() = default;
    bool push(const Event& e);
    void tick();
    void apply(const Event& e);
    void doSelect(int idx);
    void runSlice(const float* const* in, int in_ch, float* outL, float* outR, int n, int pos);
    void analyse(const float* voice_src, const float* l, const float* r, int n);
    void analysisTick();

    double sr_ = 48000;
    int max_block_ = 128;
    int in_channels_ = 1;
    uint64_t frame_ = 0;
    Arena arena_;
    std::vector<std::unique_ptr<Preset>> presets_;
    std::vector<std::pair<std::string, int>> aliases_;
    std::vector<std::unique_ptr<Node>> master_;        // the bank master (one limiter after the sum)
    std::vector<std::unique_ptr<GraphParam>> gparams_;  // all $params of all presets
    std::vector<ParamHandle> handles_;
    std::string digest_;
    bool bank_ = false;
    int active_ = 0;
    double xfade_s_ = 0.3;

    // bypass (20 ms)
    float byp_ = 0, byp_target_ = 0, byp_step_ = 0;

    // event queue (SPSC, fixed)
    static constexpr int kQueue = 256;
    Event queue_[kQueue];
    std::atomic<uint32_t> q_head_{0}, q_tail_{0};
    std::atomic<int> diag_count_{0};
    std::string diag_;

    // buffers (arena)
    float* in_[2] = {nullptr, nullptr};
    float* out_[2] = {nullptr, nullptr};
    float* mixL_ = nullptr;
    float* mixR_ = nullptr;
    float* vsrc_ = nullptr;
    float* zero_ = nullptr;

    // analysis state
    Analysis analysis_;
    float pv_ = 0, po_ = 0, a10_ = 0, ka_ = 0, kr_ = 0, voice_fast_db_ = -120, onset_ref_db_ = -120;
    float tick_peak_ = 0, peak_decay_ = 0, onset_decay_ = 0;
    int refractory_ = 0, refractory_ticks_ = 0, quiet_ticks_ = 0, hold_ticks_ = 0;
    float* fft_ring_ = nullptr;
    float* fft_re_ = nullptr;
    float* fft_im_ = nullptr;
    float* fft_win_ = nullptr;
    float* fft_cos_ = nullptr;
    float* fft_sin_ = nullptr;
    uint32_t fft_w_ = 0;
};

// One compiled Sound: chain → (buses) → trim → master, plus its $params and bindings.
class Preset {
public:
    std::string name;
    struct Section {
        std::vector<std::unique_ptr<Node>> nodes;
        std::vector<int> ch_in;   // channels entering each node
    };
    struct Bus {
        std::string name;
        Section sec;
        BusInput input{};
        int out_channels = 2;
    };
    Section chain;
    std::vector<Bus> buses;
    Section master;
    int dry_tap = 0;               // the dry is copied before chain node #dry_tap
    int in_channels = 1;
    float trim = 1;
    std::vector<GraphParam*> params;
    std::vector<Binding> bindings;
    int cost = 0;

    // runtime (bank)
    bool running = false;
    float in_g = 0, in_target = 0, in_step = 0;
    float out_g = 1, out_target = 1, out_step = 0;
    int idle_ticks = 0, quiet_ticks = 0;
    uint64_t started = 0;
    float tick_peak = 0;

    float* buf[2] = {nullptr, nullptr};
    float* dry = nullptr;
    float* gin = nullptr;    // per-sample input gain of this slice
    float* gout = nullptr;

    void tick() noexcept;
    void reset() noexcept;
    void run(const float* const* in, int in_ch, float* outL, float* outR, int n, int pos, uint64_t frame,
             std::atomic<int>& diag) noexcept;
    template <class F>
    void forEachNode(F f) {
        for (auto& n : chain.nodes) f(*n);
        for (auto& b : buses)
            for (auto& n : b.sec.nodes) f(*n);
        for (auto& n : master.nodes) f(*n);
    }
};

}  // namespace fluent_audio
