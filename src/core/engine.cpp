#include "fluent_audio/engine.hpp"

#include <algorithm>
#include <cstring>

namespace fluent_audio {

namespace {
float ease(int kind, float u) {
    switch (kind) {
        case 1: return u * u;
        case 2: return 1.0f - (1.0f - u) * (1.0f - u);
        case 3: return u < 0.5f ? 2.0f * u * u : 1.0f - 2.0f * (1.0f - u) * (1.0f - u);
        default: return u;
    }
}
inline float absf(float x) { return x < 0 ? -x : x; }
}  // namespace

// ---------------------------------------------------------------- GraphParam ($params with animate)
void GraphParam::set(float v) {
    if (!dsp::finite(v)) return;
    v = dsp::clampf(v, min, max);
    from = cur;
    to = v;
    t = 0;
    moving = true;
}

void GraphParam::tick(double dt) {
    moved = false;
    if (!moving) return;
    t += dt;
    const double u = t / dur;
    if (u >= 1.0) {
        cur = to;
        moving = false;
    } else {
        cur = from + (to - from) * ease(ease_kind, static_cast<float>(u));
    }
    moved = true;
}

// ---------------------------------------------------------------- Preset
void Preset::tick() noexcept {
    for (const Binding& b : bindings)
        if (b.gp->moved) b.node->setTarget(b.slot, static_cast<float>(b.k * b.gp->cur + b.c), 1);
    forEachNode([](Node& n) {
        n.tick();
        n.control();
    });
}

void Preset::reset() noexcept {
    forEachNode([](Node& n) {
        n.reset();
        n.forceChanged();
    });
    idle_ticks = quiet_ticks = 0;
    tick_peak = 0;
}

namespace {
inline void runSection(Preset::Section& sec, Slice& s, int from, int to, std::atomic<int>& diag) noexcept {
    for (int k = from; k < to; ++k) {
        Node& node = *sec.nodes[k];
        s.channels = sec.ch_in[k];
        node.process(s);
        const int oc = node.info()->out_channels(s.channels);
        // a node that produced NaN/Inf is reset and its slice silenced (the voice must never stay mute)
        bool bad = false;
        for (int c = 0; c < oc; ++c)
            for (int i = 0; i < s.n; ++i) bad |= !dsp::finite(s.ch[c][i]);
        if (bad) {
            node.reset();
            node.forceChanged();
            for (int c = 0; c < oc; ++c) std::memset(s.ch[c], 0, sizeof(float) * s.n);
            diag.fetch_add(1, std::memory_order_relaxed);
        }
        s.channels = oc;
    }
}
}  // namespace

void Preset::run(const float* const* in, int in_ch, float* outL, float* outR, int n, int pos, uint64_t frame,
                 std::atomic<int>& diag) noexcept {
    float* L = buf[0];
    float* R = buf[1];
    for (int i = 0; i < n; ++i) {
        const float g = gin[i];
        if (in_channels == 1) {
            L[i] = (in_ch == 2 ? 0.5f * (in[0][i] + in[1][i]) : in[0][i]) * g;
        } else {
            L[i] = in[0][i] * g;
            R[i] = (in_ch == 2 ? in[1][i] : in[0][i]) * g;
        }
    }
    for (auto& b : buses) {
        std::memset(b.input.ch[0], 0, sizeof(float) * n);
        std::memset(b.input.ch[1], 0, sizeof(float) * n);
    }
    Slice s{{L, R}, n, pos, in_channels, dry, frame};
    const int nc = static_cast<int>(chain.nodes.size());
    const int tap = dry_tap < nc ? dry_tap : nc;
    runSection(chain, s, 0, tap, diag);
    // the dry tap: after the parent chain (voice_base) — the `mix` reference and the `voice` analysis
    for (int i = 0; i < n; ++i) dry[i] = s.channels == 2 ? 0.5f * (L[i] + R[i]) : L[i];
    s.channels = tap < nc ? chain.ch_in[tap] : s.channels;
    runSection(chain, s, tap, nc, diag);
    if (s.channels == 1) std::memcpy(R, L, sizeof(float) * n);
    for (auto& b : buses) {
        Slice bs{{b.input.ch[0], b.input.ch[1]}, n, pos, b.input.channels, dry, frame};
        runSection(b.sec, bs, 0, static_cast<int>(b.sec.nodes.size()), diag);
        const float* bl = b.input.ch[0];
        const float* br = bs.channels == 2 ? b.input.ch[1] : b.input.ch[0];
        for (int i = 0; i < n; ++i) {
            L[i] += bl[i];
            R[i] += br[i];
        }
    }
    if (trim != 1.0f)
        for (int i = 0; i < n; ++i) {
            L[i] *= trim;
            R[i] *= trim;
        }
    s.channels = 2;
    runSection(master, s, 0, static_cast<int>(master.nodes.size()), diag);
    float pk = tick_peak;
    for (int i = 0; i < n; ++i) {
        const float a = L[i] * gout[i], b = R[i] * gout[i];
        outL[i] += a;
        outR[i] += b;
        pk = std::max(pk, std::max(absf(a), absf(b)));
    }
    tick_peak = pk;
}

// ---------------------------------------------------------------- Engine
Engine::~Engine() = default;

int Engine::process(int frames) {
    if (frames < 0 || frames > max_block_) return 0;
    const float* ins[2] = {in_[0], in_[1]};
    processSpan(ins, in_channels_, out_[0], out_[1], frames);
    return 1;
}

void Engine::processSpan(const float* const* in, int in_ch, float* outL, float* outR, int frames) {
    int done = 0;
    while (done < frames) {
        const int pos = static_cast<int>(frame_ % kControlRate);
        if (pos == 0) tick();
        const int n = std::min(kControlRate - pos, frames - done);
        const float* sl[2] = {in[0] + done, in[in_ch > 1 ? 1 : 0] + done};
        runSlice(sl, in_ch, outL + done, outR + done, n, pos);
        frame_ += static_cast<uint64_t>(n);
        done += n;
    }
}

void Engine::runSlice(const float* const* in_raw, int in_ch, float* outL, float* outR, int n, int pos) {
    // sanitize the input (NaN/Inf → 0)
    float* sin0 = vsrc_;   // reused below; copy first
    float raw[2][kControlRate];
    for (int c = 0; c < in_ch; ++c)
        for (int i = 0; i < n; ++i) raw[c][i] = dsp::finite(in_raw[c][i]) ? in_raw[c][i] : 0.0f;
    const float* in[2] = {raw[0], raw[in_ch > 1 ? 1 : 0]};
    float byp[kControlRate];
    for (int i = 0; i < n; ++i) {
        if (byp_ != byp_target_) {
            byp_ += byp_step_;
            if ((byp_step_ > 0 && byp_ >= byp_target_) || (byp_step_ < 0 && byp_ <= byp_target_)) byp_ = byp_target_;
        }
        byp[i] = byp_;
    }
    std::memset(mixL_, 0, sizeof(float) * n);
    std::memset(mixR_, 0, sizeof(float) * n);
    std::memset(sin0, 0, sizeof(float) * n);
    for (auto& pp : presets_) {
        Preset& p = *pp;
        if (!p.running) continue;
        for (int i = 0; i < n; ++i) {
            if (p.in_g != p.in_target) {
                p.in_g += p.in_step;
                if ((p.in_step > 0 && p.in_g >= p.in_target) || (p.in_step < 0 && p.in_g <= p.in_target)) p.in_g = p.in_target;
            }
            if (p.out_g != p.out_target) {
                p.out_g += p.out_step;
                if ((p.out_step > 0 && p.out_g >= p.out_target) || (p.out_step < 0 && p.out_g <= p.out_target)) p.out_g = p.out_target;
            }
            p.gin[i] = p.in_g * (1.0f - byp[i]);
            p.gout[i] = p.out_g;
        }
        p.run(in, in_ch, mixL_, mixR_, n, pos, frame_, diag_count_);
        if (&p == presets_[active_].get())
            for (int i = 0; i < n; ++i) sin0[i] = p.dry[i];
    }
    if (!master_.empty()) {
        Slice s{{mixL_, mixR_}, n, pos, 2, nullptr, frame_};
        for (auto& node : master_) {
            s.channels = 2;
            node->process(s);
        }
    }
    for (int i = 0; i < n; ++i) {
        const float x = in_ch == 2 ? 0.5f * (in[0][i] + in[1][i]) : in[0][i];
        const float b = byp[i];
        outL[i] = mixL_[i] + (in_ch == 2 ? in[0][i] : x) * b;
        outR[i] = mixR_[i] + (in_ch == 2 ? in[1][i] : x) * b;
        sin0[i] += x * b;
    }
    analyse(sin0, outL, outR, n);
}

// ---- control tick (absolute frame % 32 == 0) ----
void Engine::tick() {
    // events due by now, in arrival order
    for (;;) {
        const uint32_t t = q_tail_.load(std::memory_order_relaxed);
        if (t == q_head_.load(std::memory_order_acquire)) break;
        const Event& e = queue_[t % kQueue];
        if (e.frame != 0 && e.frame > frame_) break;
        if (e.frame != 0 && e.frame + kControlRate <= frame_) diag_count_.fetch_add(1, std::memory_order_relaxed);   // late
        apply(e);
        q_tail_.store(t + 1, std::memory_order_release);
    }
    const double dt = static_cast<double>(kControlRate) / sr_;
    for (auto& gp : gparams_) gp->tick(dt);
    const int tail_ticks = static_cast<int>(2.0 * sr_ / kControlRate);
    for (size_t k = 0; k < presets_.size(); ++k) {
        Preset& p = *presets_[k];
        if (!p.running) continue;
        if (static_cast<int>(k) != active_) {
            // a previous preset: its input is faded out; it stops when the tail is quiet (or after 2 s)
            if (p.in_g == 0.0f && p.in_target == 0.0f) {
                ++p.idle_ticks;
                p.quiet_ticks = p.tick_peak < 0.001f ? p.quiet_ticks + 1 : 0;
                if (p.quiet_ticks >= hold_ticks_ || p.idle_ticks >= tail_ticks) p.running = false;
            }
            if (p.out_g == 0.0f && p.out_target == 0.0f) p.running = false;
            if (!p.running) continue;
        }
        p.tick_peak = 0;
        p.tick();
    }
    for (auto& n : master_) {
        n->tick();
        n->control();
    }
    analysisTick();
}

bool Engine::push(const Event& e) {
    const uint32_t h = q_head_.load(std::memory_order_relaxed);
    if (h - q_tail_.load(std::memory_order_acquire) >= static_cast<uint32_t>(kQueue)) {
        diag_count_.fetch_add(1, std::memory_order_relaxed);
        diag_ = "event queue full (256)";
        return false;
    }
    queue_[h % kQueue] = e;
    q_head_.store(h + 1, std::memory_order_release);
    return true;
}

void Engine::apply(const Event& e) {
    switch (e.kind) {
        case 0: {
            const ParamHandle& h = handles_[e.idx];
            for (const auto& t : h.targets) {
                if (t.gp) t.gp->set(e.value);
                else if (t.node) t.node->setTarget(t.slot, static_cast<float>(t.k * e.value + t.c), 0);
            }
            break;
        }
        case 1: doSelect(e.idx); break;
        case 2:
            byp_target_ = e.value > 0.5f ? 1.0f : 0.0f;
            byp_step_ = (byp_target_ > byp_ ? 1.0f : -1.0f) / static_cast<float>(0.020 * sr_);
            break;
        case 3:
            for (auto& p : presets_) {
                p->reset();
                p->running = false;
            }
            {
                Preset& a = *presets_[active_];
                a.running = true;
                a.in_g = a.in_target = 1;
                a.out_g = a.out_target = 1;
            }
            for (auto& n : master_) { n->reset(); n->forceChanged(); }
            break;
        default: break;
    }
}

void Engine::doSelect(int idx) {
    if (idx < 0 || idx >= static_cast<int>(presets_.size())) return;
    Preset& nw = *presets_[idx];
    if (idx == active_ && nw.running && nw.in_target == 1.0f) return;
    if (frame_ == 0) {   // before the first sample: switch at once, nothing to crossfade
        for (auto& p : presets_) p->running = false;
        nw.reset();
        nw.running = true;
        nw.in_g = nw.in_target = 1;
        nw.out_g = nw.out_target = 1;
        active_ = idx;
        return;
    }
    const float xstep = 1.0f / static_cast<float>(xfade_s_ * sr_);
    const float kill = 1.0f / static_cast<float>(0.050 * sr_);
    for (size_t k = 0; k < presets_.size(); ++k) {   // older tails: fade out in 50 ms and stop
        Preset& p = *presets_[k];
        if (!p.running || static_cast<int>(k) == active_ || static_cast<int>(k) == idx) continue;
        p.out_target = 0;
        p.out_step = -kill;
    }
    Preset& old = *presets_[active_];
    if (active_ != idx) {
        old.in_target = 0;
        old.in_step = -xstep;
        old.idle_ticks = old.quiet_ticks = 0;
    }
    if (!nw.running) {
        nw.reset();
        nw.running = true;
        nw.in_g = 0;
        nw.out_g = 1;
    }
    nw.in_target = 1;
    nw.in_step = xstep;
    nw.out_target = 1;
    nw.out_step = kill;
    nw.started = frame_;
    active_ = idx;
}

// ---- analysis (design §6) ----
void Engine::analyse(const float* v, const float* l, const float* r, int n) {
    const float k = 1.0f - a10_;
    float pv = pv_, po = po_, pk = tick_peak_;
    for (int i = 0; i < n; ++i) {
        pv = dsp::flush(pv + (v[i] * v[i] - pv) * k);
        const float o = 0.5f * (l[i] * l[i] + r[i] * r[i]);
        po = dsp::flush(po + (o - po) * k);
        pk = std::max(pk, std::max(absf(l[i]), absf(r[i])));
        fft_ring_[fft_w_ & 255] = 0.5f * (l[i] + r[i]);
        ++fft_w_;
    }
    pv_ = pv;
    po_ = po;
    tick_peak_ = pk;
}

void Engine::analysisTick() {
    Analysis& a = analysis_;
    auto norm = [](double db) { return dsp::clampf(static_cast<float>((db + 60.0) / 54.0), 0.0f, 1.0f); };
    const double vdb = 0.5 * dsp::lin2db(static_cast<double>(pv_) + 1e-12);
    const double odb = 0.5 * dsp::lin2db(static_cast<double>(po_) + 1e-12);
    const float tv = norm(vdb), to = norm(odb);
    a.voice = dsp::flush(tv + (tv > a.voice ? ka_ : kr_) * (a.voice - tv));
    a.level = dsp::flush(to + (to > a.level ? ka_ : kr_) * (a.level - to));
    a.peak = std::max(a.peak * peak_decay_, tick_peak_);
    tick_peak_ = 0;
    // onset: the fast voice level jumps 9 dB over its slower reference (60 ms refractory)
    const float vf = static_cast<float>(vdb);
    if (refractory_ > 0) --refractory_;
    if (vf > -50.0f && vf - onset_ref_db_ > 9.0f && refractory_ == 0) {
        a.onset = 1.0f;
        refractory_ = refractory_ticks_;
    } else {
        a.onset = dsp::flush(a.onset * onset_decay_);
    }
    onset_ref_db_ += (vf - onset_ref_db_) * 0.05f;
    // tail: 1 while the output is above -60 dBFS (re-arms at -54), held 100 ms
    if (odb > -54.0) { a.tail = 1.0f; quiet_ticks_ = 0; }
    else if (odb < -60.0) {
        if (++quiet_ticks_ >= hold_ticks_) a.tail = 0.0f;
    }
    // centroid: 256-point FFT of the output every 8 ticks (256 samples)
    if (((frame_ / kControlRate) & 7) == 0 && odb > -70.0) {
        for (int i = 0; i < 256; ++i) {
            fft_re_[i] = fft_ring_[(fft_w_ + i) & 255] * fft_win_[i];
            fft_im_[i] = 0;
        }
        for (uint32_t i = 1, j = 0; i < 256; ++i) {   // bit reversal
            uint32_t bit = 128;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(fft_re_[i], fft_re_[j]); std::swap(fft_im_[i], fft_im_[j]); }
        }
        for (int len = 2; len <= 256; len <<= 1) {
            const int half = len >> 1, step = 256 / len;
            for (int i = 0; i < 256; i += len)
                for (int j = 0; j < half; ++j) {
                    const float wr = fft_cos_[j * step], wi = fft_sin_[j * step];
                    const float xr = fft_re_[i + j + half] * wr - fft_im_[i + j + half] * wi;
                    const float xi = fft_re_[i + j + half] * wi + fft_im_[i + j + half] * wr;
                    fft_re_[i + j + half] = fft_re_[i + j] - xr;
                    fft_im_[i + j + half] = fft_im_[i + j] - xi;
                    fft_re_[i + j] += xr;
                    fft_im_[i + j] += xi;
                }
        }
        double num = 0, den = 0;
        for (int b = 1; b < 128; ++b) {
            const double m = std::sqrt(static_cast<double>(fft_re_[b] * fft_re_[b] + fft_im_[b] * fft_im_[b]));
            num += m * b;
            den += m;
        }
        if (den > 1e-6) {
            const double hz = num / den * sr_ / 256.0;
            const double c = dsp::dlog(std::max(hz, 1.0) / 300.0) / dsp::dlog(5000.0 / 300.0);
            const float cf = dsp::clampf(static_cast<float>(c), 0.0f, 1.0f);
            a.centroid += (cf - a.centroid) * 0.3f;
        }
    }
    a.frame = frame_ > 0 ? frame_ - 1 : 0;
}

// ---- control API ----
int Engine::paramIndex(const std::string& name) const {
    for (size_t i = 0; i < handles_.size(); ++i)
        if (handles_[i].name == name) return static_cast<int>(i);
    return -1;
}

bool Engine::setParamIdx(int idx, float v) { return setParamAt(idx, v, 0); }

bool Engine::setParamAt(int idx, float v, uint64_t frame) {
    if (idx < 0 || idx >= static_cast<int>(handles_.size())) { diag_ = "no such param index"; return false; }
    ParamHandle& h = handles_[idx];
    if (!h.runtime) { diag_ = h.name + " is fixed at compile time"; return false; }
    if (!dsp::finite(v)) { diag_ = "not a finite number"; return false; }
    if (v < h.min || v > h.max) diag_ = h.name + ": clamped to [" + std::to_string(h.min) + ", " + std::to_string(h.max) + "]";
    h.value = dsp::clampf(v, h.min, h.max);
    return push({frame, 0, idx, h.value});
}

bool Engine::setParam(const std::string& name, float v) {
    const int i = paramIndex(name);
    if (i < 0) { diag_ = "no param '" + name + "'"; return false; }
    return setParamIdx(i, v);
}

float Engine::getParam(const std::string& name) const {
    const int i = paramIndex(name);
    return i < 0 ? 0.0f : handles_[i].value;
}

int Engine::presetIndex(const std::string& name) const {
    for (size_t i = 0; i < presets_.size(); ++i)
        if (presets_[i]->name == name) return static_cast<int>(i);
    for (const auto& a : aliases_)
        if (a.first == name) return a.second;
    return -1;
}
int Engine::presetCount() const { return static_cast<int>(presets_.size()); }
const std::string& Engine::presetName(int i) const { return presets_[i]->name; }

bool Engine::select(const std::string& name) {
    const int i = presetIndex(name);
    if (i < 0) { diag_ = "no preset '" + name + "'"; return false; }
    return selectIdx(i);
}
bool Engine::selectIdx(int idx) { return selectIdxAt(idx, 0); }
bool Engine::selectIdxAt(int idx, uint64_t frame) {
    if (idx < 0 || idx >= presetCount()) { diag_ = "no such preset index"; return false; }
    return push({frame, 1, idx, 0});
}
bool Engine::bypass(bool on) { return push({0, 2, 0, on ? 1.0f : 0.0f}); }
void Engine::reset() { push({0, 3, 0, 0}); }

std::string Engine::takeDiagnostics() {
    std::string d;
    d.swap(diag_);
    return d;
}

size_t Engine::memoryBytes() const { return arena_.bytes(); }

std::string Engine::describe() const {
    std::string s;
    for (const auto& p : presets_) {
        s += p->name + " (cost ~" + std::to_string(p->cost) + ", dry tap after " + std::to_string(p->dry_tap) + " nodes)\n  chain:";
        for (const auto& n : p->chain.nodes) s += " " + n->id;
        for (const auto& b : p->buses) {
            s += "\n  bus " + b.name + ":";
            for (const auto& n : b.sec.nodes) s += " " + n->id;
        }
        s += "\n  master:";
        for (const auto& n : p->master.nodes) s += " " + n->id;
        s += "\n";
    }
    if (!master_.empty()) {
        s += "bank master:";
        for (const auto& n : master_) s += " " + n->id;
        s += "\n";
    }
    return s;
}

}  // namespace fluent_audio
