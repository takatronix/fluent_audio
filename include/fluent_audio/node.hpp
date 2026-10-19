#pragma once
// A node = one effect in a file of its own (src/nodes/<name>.cpp). It declares a parameter table and three or
// four methods, and registers itself with one line:
//
//     class Gain final : public Node {
//      public:
//       enum { kDb };
//       static constexpr ParamSpec kParams[] = {{"db", "dB", -60, 24, 0, Smooth::kLinear, "gain"}};
//       void control() noexcept override { ... }        // every 32 samples, after the params moved
//       void process(Slice& s) noexcept override { ... } // the samples (n <= 32)
//     };
//     FA_NODE(gain, Gain, "Gain in dB.")
//
// The YAML loader, the C API (set/get by name), the wasm/JS wrapper, Python and the generated docs all read the
// table, so nothing else has to change (src/nodes/ is globbed by the build). See docs/howto-add-filter.ja.md.
//
// Real-time rules, held by this base class:
//  - memory comes only from Prepare::arena (prepare() is the only place that sees it; the arena is sealed before
//    the first process call, so a late allocation returns nullptr and fails the compile);
//  - control()/process()/reset() are noexcept and get no allocator, no strings, no I/O;
//  - the graph checks every node's output for NaN/Inf after each slice, resets the node and counts it;
//  - the test suite runs every registered node under a malloc/new hook (0 calls in process), with its params at
//    the range ends, chunked at random sizes (bit-identical output) — new nodes get these tests for free.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fluent_audio/dsp.hpp"

namespace fluent_audio {

constexpr int kControlRate = 32;   // samples per control tick (design D4); ticks fall on absolute frame % 32 == 0

enum class Smooth : uint8_t {
    kFixed,    // set at compile time only (types, voice counts, buffer sizes)
    kLinear,   // runtime: linear ramp (>= 20 ms)
    kLog,      // runtime: ramp in the log domain (frequencies, Q)
};

struct ParamSpec {
    const char* name;
    const char* unit;       // "Hz", "dB", "ms", "s", "st", "" ...
    float min, max, def;
    Smooth smooth;
    const char* help;
    const char* choices = nullptr;   // "hp|lp|peak": an enum; the value is the index
    const char* group = nullptr;     // params sharing a group come as a YAML list of maps: bands: [{type, f}, ...]
};

// Prepare-time allocator. Every buffer a node needs comes from here, sized from the param ranges.
class Arena {
public:
    template <class T>
    T* alloc(size_t n) { return static_cast<T*>(allocBytes(n * sizeof(T))); }
    void* allocBytes(size_t bytes);
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }
    size_t bytes() const { return bytes_; }

private:
    std::vector<std::unique_ptr<std::max_align_t[]>> blocks_;
    size_t bytes_ = 0;
    bool sealed_ = false;
};

struct Prepare {
    double sample_rate;
    int channels;          // channels coming into the node (1 or 2)
    Arena& arena;
    std::string& error;    // set it (and return) to reject the configuration
};

// One run of samples inside one control tick. ch[0] is left (or mono), ch[1] right (always valid memory; holds
// a signal only when channels == 2). A node that widens mono to stereo writes both and the graph notes it.
struct Slice {
    float* ch[2];
    int n;                 // 1..32 samples
    int pos;               // offset of the first sample within the current control tick (0..31)
    int channels;          // channels entering the node
    const float* dry;      // the preset's dry tap for these samples (mono) — used by `mix`
    uint64_t frame;        // absolute frame number of the first sample
};

// The input of a bus for the current slice (filled by `send` nodes, consumed by the bus's chain).
struct BusInput {
    float* ch[2];
    int channels;
};

struct NodeInfo;

class Node {
public:
    virtual ~Node() = default;

    // ---- what a node writes ----
    virtual void prepare(Prepare&) {}            // allocate (only here) and size the state
    virtual void reset() noexcept {}             // back to silence
    virtual void control() noexcept {}           // once per tick (32 samples) after the params moved
    virtual void process(Slice& s) noexcept = 0;  // RT: no allocation, locks, syscalls, exceptions, strings

    // Defaults a node may shadow with its own static members.
    static constexpr int kMaxItems = 1;          // max entries of a grouped param list (eq bands: 8)
    static constexpr int kCost = 5;              // rough ops per sample (design §4), for the budget check
    static int outChannels(int in) { return in; }

    // ---- what a node reads ----
    float p(int param, int item = 0) const { return slots_[offsets_[param] + item].cur; }
    float prevP(int param, int item = 0) const { return slots_[offsets_[param] + item].prev; }
    bool changed() const { return changed_; }                       // any param moved this tick (or first tick)
    bool changed(int param, int item = 0) const { return changed_all_ || slots_[offsets_[param] + item].moved; }
    int items() const { return items_; }                            // entries in the grouped list
    double sampleRate() const { return sr_; }
    // Per-sample linear interpolation inside a tick (gains): value at sample k of the slice.
    static float tickLerp(float from, float to, int pos) {
        return from + (to - from) * (static_cast<float>(pos + 1) * (1.0f / kControlRate));
    }

    virtual void connectBus(BusInput*) {}       // only `send` uses it

    // ---- graph side (node authors do not call these) ----
    struct Slot {
        float cur = 0, prev = 0, target = 0, inc = 0;
        int left = 0;
        Smooth smooth = Smooth::kFixed;
        bool moved = false;
        float min = 0, max = 0;
    };
    void init(const NodeInfo* info, int items, double sr);
    const NodeInfo* info() const { return info_; }
    int slotCount() const { return static_cast<int>(slots_.size()); }
    int slotOf(int param, int item) const { return offsets_[param] + item; }
    const Slot& slot(int i) const { return slots_[i]; }
    void setInitial(int slot, float v);                 // compile time: no smoothing
    bool setTarget(int slot, float v, int ramp_ticks);  // runtime; ramp_ticks <= 0: the default (20 ms)
    void tick() noexcept;                               // advance the smoothers; then the graph calls control()
    void forceChanged() { force_next_ = true; }        // next tick reports every param as moved
    std::string id;                                     // "eq1", "room.reverb1", or the YAML `id:`

private:
    const NodeInfo* info_ = nullptr;
    std::vector<Slot> slots_;
    std::vector<int> offsets_;
    int items_ = 1;
    double sr_ = 48000;
    int default_ramp_ = 30;
    bool changed_ = true, changed_all_ = true, force_next_ = true;
};

struct NodeInfo {
    const char* name;
    const char* help;
    const ParamSpec* params;
    int nparams;
    int max_items;
    int cost;
    Node* (*create)();
    int (*out_channels)(int);
    const char* file;
};

const std::vector<const NodeInfo*>& nodeRegistry();
const NodeInfo* findNode(const std::string& name);
struct NodeRegistrar {
    explicit NodeRegistrar(const NodeInfo* info);
};

}  // namespace fluent_audio

// Registers CLASS as the node type NAME (the YAML key). One line at the end of the node's file.
#define FA_NODE(NAME, CLASS, HELP)                                                                          \
    static const ::fluent_audio::NodeInfo fa_node_info_##NAME = {                                           \
        #NAME, HELP, CLASS::kParams, static_cast<int>(sizeof(CLASS::kParams) / sizeof(CLASS::kParams[0])), \
        CLASS::kMaxItems, CLASS::kCost, []() -> ::fluent_audio::Node* { return new CLASS(); },             \
        &CLASS::outChannels, __FILE__};                                                                     \
    static const ::fluent_audio::NodeRegistrar fa_node_registrar_##NAME(&fa_node_info_##NAME);
