#include "fluent_audio/fa.h"

#include <cstring>
#include <string>

#include "fluent_audio/engine.hpp"

using fluent_audio::Engine;

struct FaInstance {
    std::unique_ptr<Engine> eng;
    std::string str;   // storage for returned strings
    FaAnalysis an{};
};

namespace {
std::string g_error;
std::string g_json;

std::string jsonEscape(const char* s) {
    std::string o;
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') { o += '\\'; o += *s; }
        else if (*s == '\n') o += "\\n";
        else o += *s;
    }
    return o;
}
std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}
}  // namespace

extern "C" {

FaInstance* fa_create_ex(const char* yaml, float sr, int max_block, const char* dir) {
    if (!yaml) { g_error = "yaml is NULL"; return nullptr; }
    fluent_audio::Options o;
    o.sample_rate = sr;
    o.max_block = max_block;
    if (dir) o.search_dir = dir;
    auto e = Engine::create(yaml, o, g_error);
    if (!e) return nullptr;
    auto* f = new FaInstance();
    f->eng = std::move(e);
    return f;
}
FaInstance* fa_create(const char* yaml, float sr, int max_block) { return fa_create_ex(yaml, sr, max_block, nullptr); }
const char* fa_error(void) { return g_error.c_str(); }
void fa_destroy(FaInstance* f) { delete f; }
const char* fa_digest(FaInstance* f) { return f ? f->eng->digest().c_str() : nullptr; }
int fa_latency_samples(FaInstance* f) { return f ? f->eng->latencySamples() : 0; }
int fa_memory_bytes(FaInstance* f) { return f ? static_cast<int>(f->eng->memoryBytes()) : 0; }

float* fa_in_buffer(FaInstance* f, int ch) { return f && ch >= 0 && ch < 2 ? f->eng->in(ch) : nullptr; }
float* fa_out_buffer(FaInstance* f, int ch) { return f && ch >= 0 && ch < 2 ? f->eng->out(ch) : nullptr; }
int fa_in_channels(FaInstance* f) { return f ? f->eng->inChannels() : 0; }
int fa_process(FaInstance* f, int frames) { return f ? f->eng->process(frames) : 0; }

int fa_set_param_f32(FaInstance* f, const char* n, float v) { return f && n && f->eng->setParam(n, v) ? 1 : 0; }
int fa_set_param_bool(FaInstance* f, const char* n, int v) { return fa_set_param_f32(f, n, v ? 1.0f : 0.0f); }
float fa_get_param_f32(FaInstance* f, const char* n) { return f && n ? f->eng->getParam(n) : 0.0f; }
int fa_param_index(FaInstance* f, const char* n) { return f && n ? f->eng->paramIndex(n) : -1; }
int fa_param_count(FaInstance* f) { return f ? f->eng->paramCount() : 0; }
const char* fa_param_name(FaInstance* f, int i) {
    return f && i >= 0 && i < f->eng->paramCount() ? f->eng->param(i).name.c_str() : nullptr;
}
float fa_param_min(FaInstance* f, int i) { return f && i >= 0 && i < f->eng->paramCount() ? f->eng->param(i).min : 0.0f; }
float fa_param_max(FaInstance* f, int i) { return f && i >= 0 && i < f->eng->paramCount() ? f->eng->param(i).max : 0.0f; }
int fa_set_param_idx(FaInstance* f, int i, float v) { return f && f->eng->setParamIdx(i, v) ? 1 : 0; }
int fa_set_param_at(FaInstance* f, int i, float v, uint64_t frame) { return f && f->eng->setParamAt(i, v, frame) ? 1 : 0; }
int fa_set_param_at_f64(FaInstance* f, int i, float v, double frame) {
    return fa_set_param_at(f, i, v, frame < 0 ? 0 : static_cast<uint64_t>(frame));
}

int fa_select(FaInstance* f, const char* p) { return f && p && f->eng->select(p) ? 1 : 0; }
int fa_preset_index(FaInstance* f, const char* p) { return f && p ? f->eng->presetIndex(p) : -1; }
int fa_preset_count(FaInstance* f) { return f ? f->eng->presetCount() : 0; }
const char* fa_preset_name(FaInstance* f, int i) {
    return f && i >= 0 && i < f->eng->presetCount() ? f->eng->presetName(i).c_str() : nullptr;
}
int fa_select_idx(FaInstance* f, int i) { return f && f->eng->selectIdx(i) ? 1 : 0; }
int fa_select_at(FaInstance* f, int i, uint64_t frame) { return f && f->eng->selectIdxAt(i, frame) ? 1 : 0; }
int fa_current_preset(FaInstance* f) { return f ? f->eng->current() : -1; }
int fa_bypass(FaInstance* f, int on) { return f && f->eng->bypass(on != 0) ? 1 : 0; }
void fa_reset(FaInstance* f) { if (f) f->eng->reset(); }

const FaAnalysis* fa_analysis(FaInstance* f) {
    if (!f) return nullptr;
    static_assert(sizeof(FaAnalysis) == sizeof(fluent_audio::Analysis), "layout");
    std::memcpy(&f->an, &f->eng->analysis(), sizeof(FaAnalysis));
    return &f->an;
}
int fa_diagnostics_count(FaInstance* f) { return f ? f->eng->diagnosticsCount() : 0; }
const char* fa_diagnostics(FaInstance* f) {
    if (!f) return nullptr;
    f->str = f->eng->takeDiagnostics();
    return f->str.c_str();
}
const char* fa_drain_events(FaInstance* f) { return f ? "" : nullptr; }
double fa_frame(FaInstance* f) { return f ? static_cast<double>(f->eng->frame()) : 0.0; }

const char* fa_nodes_json(void) {
    std::string j = "[";
    for (const fluent_audio::NodeInfo* n : fluent_audio::nodeRegistry()) {
        if (j.size() > 1) j += ",";
        j += "{\"name\":\"" + jsonEscape(n->name) + "\",\"help\":\"" + jsonEscape(n->help) + "\",\"cost\":" +
             std::to_string(n->cost) + ",\"params\":[";
        for (int i = 0; i < n->nparams; ++i) {
            const fluent_audio::ParamSpec& p = n->params[i];
            if (i) j += ",";
            j += "{\"name\":\"" + jsonEscape(p.name) + "\",\"unit\":\"" + jsonEscape(p.unit) + "\",\"min\":" + num(p.min) +
                 ",\"max\":" + num(p.max) + ",\"default\":" + num(p.def) + ",\"runtime\":" +
                 (p.smooth == fluent_audio::Smooth::kFixed ? "false" : "true") + ",\"help\":\"" + jsonEscape(p.help) + "\"";
            if (p.choices) j += ",\"choices\":\"" + jsonEscape(p.choices) + "\"";
            if (p.group) j += ",\"group\":\"" + jsonEscape(p.group) + "\"";
            j += "}";
        }
        j += "]}";
    }
    j += "]";
    g_json = j;
    return g_json.c_str();
}

const char* fa_version(void) { return "fluent_audio 0.1.0 (M1)"; }

}  // extern "C"
