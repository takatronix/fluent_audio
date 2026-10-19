#include "fluent_audio/node.hpp"

#include <algorithm>
#include <cstring>

namespace fluent_audio {

void* Arena::allocBytes(size_t bytes) {
    if (sealed_) return nullptr;   // a node asked for memory after compile: the graph refuses to run it
    const size_t n = (bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    blocks_.emplace_back(new std::max_align_t[n > 0 ? n : 1]);
    std::memset(blocks_.back().get(), 0, (n > 0 ? n : 1) * sizeof(std::max_align_t));
    bytes_ += n * sizeof(std::max_align_t);
    return blocks_.back().get();
}

void Node::init(const NodeInfo* info, int items, double sr) {
    info_ = info;
    items_ = items < 1 ? 1 : items;
    sr_ = sr;
    default_ramp_ = std::max(1, static_cast<int>(0.020 * sr / kControlRate + 0.5));
    offsets_.assign(info->nparams, 0);
    int n = 0;
    for (int i = 0; i < info->nparams; ++i) {
        offsets_[i] = n;
        n += info->params[i].group ? info->max_items : 1;
    }
    slots_.assign(n, Slot{});
    for (int i = 0; i < info->nparams; ++i) {
        const ParamSpec& ps = info->params[i];
        const int cnt = ps.group ? info->max_items : 1;
        for (int k = 0; k < cnt; ++k) {
            Slot& s = slots_[offsets_[i] + k];
            s.smooth = ps.smooth;
            s.min = ps.min;
            s.max = ps.max;
            s.cur = s.prev = s.target = ps.def;
        }
    }
}

void Node::setInitial(int slot, float v) {
    Slot& s = slots_[slot];
    v = dsp::clampf(v, s.min, s.max);
    s.cur = s.prev = s.target = v;
    s.left = 0;
}

bool Node::setTarget(int slot, float v, int ramp_ticks) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return false;
    Slot& s = slots_[slot];
    if (s.smooth == Smooth::kFixed) return false;
    if (!dsp::finite(v)) return false;
    v = dsp::clampf(v, s.min, s.max);
    s.target = v;
    const int n = ramp_ticks > 0 ? ramp_ticks : default_ramp_;
    if (s.smooth == Smooth::kLog && s.cur > 0.0f && v > 0.0f) {
        s.inc = static_cast<float>(dsp::dexp(dsp::dlog(static_cast<double>(v) / s.cur) / n));
    } else {
        s.inc = (v - s.cur) / static_cast<float>(n);
    }
    s.left = n;
    return true;
}

void Node::tick() noexcept {
    changed_all_ = force_next_;
    force_next_ = false;
    bool any = changed_all_;
    for (Slot& s : slots_) {
        s.prev = s.cur;
        s.moved = false;
        if (s.left > 0) {
            if (--s.left == 0) s.cur = s.target;
            else if (s.smooth == Smooth::kLog && s.cur > 0.0f && s.target > 0.0f) s.cur *= s.inc;
            else s.cur += s.inc;
            s.moved = true;
            any = true;
        }
    }
    changed_ = any;
}

const std::vector<const NodeInfo*>& nodeRegistry() {
    static std::vector<const NodeInfo*>* reg = new std::vector<const NodeInfo*>();
    return *reg;
}

NodeRegistrar::NodeRegistrar(const NodeInfo* info) {
    auto& reg = const_cast<std::vector<const NodeInfo*>&>(nodeRegistry());
    auto it = std::lower_bound(reg.begin(), reg.end(), info,
                               [](const NodeInfo* a, const NodeInfo* b) { return std::strcmp(a->name, b->name) < 0; });
    reg.insert(it, info);
}

const NodeInfo* findNode(const std::string& name) {
    for (const NodeInfo* i : nodeRegistry())
        if (name == i->name) return i;
    return nullptr;
}

}  // namespace fluent_audio
