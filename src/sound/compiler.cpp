// Sound YAML → Engine. Everything that can be wrong is rejected here, with file:line, before any audio runs.
//
// Name lookup for a chain entry `- <type>: {...}`: a registered C++ node (src/nodes/), else a macro (a YAML file
// with `macro: <type>`: a named sub-chain with its own params) from the pack's `library:`, the search dir
// (<dir>/<type>.yaml, <dir>/macros/<type>.yaml) or the built-in macros (macros/*.yaml, embedded at build time).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "fluent_audio/engine.hpp"
#include "fluent_audio/yaml.hpp"

namespace fluent_audio {

using yaml::YamlNode;

// generated from macros/*.yaml (tools/embed_macros.py)
extern const char* const kBuiltinMacros[][2];
extern const int kBuiltinMacroCount;

namespace {

constexpr int kMaxNodes = 64, kMaxBuses = 8, kMaxParams = 64, kMaxDepth = 2, kMaxMacroDepth = 4, kBudget = 600;
constexpr size_t kMaxYaml = 256 * 1024;   // a pack holds all looks; one Sound stays far below the 64 KB of §5.1

struct Origin {
    std::string handle;
    double k = 1, c = 0;
};

// An affine value: num (+ k·$params.<gparam> when bound). Origins tie it to macro params set at runtime.
struct Val {
    double num = 0, k = 1;
    std::string gparam;
    std::vector<Origin> origins;
    std::string text;
    bool is_text = false;
    bool bound() const { return !gparam.empty(); }
};

bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

std::string fmt(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}

struct Ctx {
    std::map<std::string, Val> vars;   // macro params
    const Ctx* parent = nullptr;
};

}  // namespace

class Compiler {
public:
    Compiler(const Options& opt, std::string& err) : opt_(opt), err_(err) {}

    std::unique_ptr<Engine> build(const std::string& text) {
        if (text.size() > kMaxYaml) return fail("", 0, "YAML larger than 256 KB");
        Doc* root = parseDoc(text, "<input>");
        if (!root) return nullptr;
        if (const YamlNode* lib = root->root.find("library")) {
            if (!lib->isMapping()) return fail(root->file, lib->span.begin_line, "library: must be a mapping of name: document");
            for (const auto& e : lib->entries) lib_[e.key] = {&e.value, "library." + e.key};
        }
        eng_.reset(new Engine());
        eng_->sr_ = opt_.sample_rate;
        eng_->max_block_ = opt_.max_block;
        if (opt_.sample_rate < 22050 || opt_.sample_rate > 96000) return fail("", 0, "sample rate must be 22050..96000");
        if (opt_.max_block < 1 || opt_.max_block > 8192) return fail("", 0, "max_block must be 1..8192");

        const YamlNode& r = root->root;
        if (!r.isMapping()) return fail(root->file, 1, "a Sound document must be a mapping");
        if (!checkSchema(r, root->file)) return nullptr;
        std::vector<std::pair<std::string, const YamlNode*>> presets;   // name, document
        std::vector<std::string> files;
        const YamlNode* bank_master = nullptr;
        if (r.find("bank")) {
            if (!allowKeys(r, root->file, {"schema", "bank", "presets", "aliases", "crossfade", "master", "library", "help"}))
                return nullptr;
            eng_->bank_ = true;
            const YamlNode* ps = r.find("presets");
            if (!ps || !ps->isSequence() || ps->elements.empty())
                return fail(root->file, r.span.begin_line, "bank: needs presets: [name, ...]");
            for (const auto& e : ps->elements) {
                if (!e.isScalar()) return fail(root->file, e.span.begin_line, "presets: entries are names");
                std::string file;
                const YamlNode* d = findDoc(e.scalar, file);
                if (!d) return fail(root->file, e.span.begin_line, "preset '" + e.scalar + "' not found (library / search dir)");
                presets.push_back({e.scalar, d});
                files.push_back(file);
            }
            if (const YamlNode* cf = r.find("crossfade")) {
                if (const YamlNode* d = cf->find("duration")) {
                    double v;
                    if (!parseNumber(d->scalar, v) || v < 0.005 || v > 5) return fail(root->file, d->span.begin_line, "crossfade.duration: 0.005..5 s");
                    eng_->xfade_s_ = v;
                }
            }
            bank_master = r.find("master");
        } else {
            std::string name = r.find("name") && r.find("name")->isScalar() ? r.find("name")->scalar : "sound";
            presets.push_back({name, &r});
            files.push_back(root->file);
        }

        // buffers
        Arena& a = eng_->arena_;
        const int mb = opt_.max_block;
        for (int c = 0; c < 2; ++c) {
            eng_->in_[c] = a.alloc<float>(mb);
            eng_->out_[c] = a.alloc<float>(mb);
        }
        eng_->mixL_ = a.alloc<float>(kControlRate);
        eng_->mixR_ = a.alloc<float>(kControlRate);
        eng_->vsrc_ = a.alloc<float>(kControlRate);
        eng_->zero_ = a.alloc<float>(kControlRate);

        for (size_t i = 0; i < presets.size(); ++i) {
            if (!buildPreset(presets[i].first, presets[i].second, files[i])) return nullptr;
            eng_->in_channels_ = std::max(eng_->in_channels_, eng_->presets_.back()->in_channels);
        }
        if (const YamlNode* al = r.find("aliases")) {
            if (!al->isMapping()) return fail(root->file, al->span.begin_line, "aliases: { look: preset }");
            for (const auto& e : al->entries) {
                const int idx = eng_->presetIndex(e.value.scalar);
                if (idx < 0) return fail(root->file, e.value.span.begin_line, "alias '" + e.key + "' → unknown preset '" + e.value.scalar + "'");
                eng_->aliases_.push_back({e.key, idx});
            }
        }
        if (bank_master) {
            Preset::Section sec;
            int ch = 2;
            Preset dummy;
            cur_ = &dummy;
            Ctx ctx;
            Counters cnt;
            if (!bank_master->isSequence()) return fail(root->file, bank_master->span.begin_line, "master: is a list");
            for (const auto& item : bank_master->elements)
                if (!addEntry(sec, ch, item, root->file, ctx, "", cnt, 0, Where::kMaster)) return nullptr;
            for (auto& n : sec.nodes) eng_->master_.push_back(std::move(n));
            if (!dummy.bindings.empty()) return fail(root->file, bank_master->span.begin_line, "the bank master cannot use $params");
            cur_ = nullptr;
        }
        setupAnalysis();
        eng_->arena_.seal();   // from here on no node can get memory

        // initial state: the first preset plays
        Preset& p0 = *eng_->presets_[0];
        p0.running = true;
        p0.in_g = p0.in_target = 1;
        eng_->active_ = 0;
        for (auto& p : eng_->presets_) p->reset();
        for (auto& n : eng_->master_) { n->reset(); n->forceChanged(); }

        // digest: FNV-1a over the canonical structure (independent of key order / spacing / comments)
        uint64_t h = 1469598103934665603ull;
        for (unsigned char ch : canon_) { h ^= ch; h *= 1099511628211ull; }
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        eng_->digest_ = hex;
        return std::move(eng_);
    }

private:
    struct Doc {
        YamlNode root;
        std::string file;
    };
    enum class Where { kChain, kBus, kMaster };
    using Counters = std::map<std::string, int>;

    std::unique_ptr<Engine> fail(const std::string& file, uint32_t line, const std::string& msg) {
        err_ = (file.empty() ? "" : file + ":" + std::to_string(line) + ": ") + msg;
        return nullptr;
    }
    bool err(const std::string& file, uint32_t line, const std::string& msg) {
        fail(file, line, msg);
        return false;
    }

    Doc* parseDoc(const std::string& text, const std::string& file) {
        yaml::DiagnosticList dl;
        auto d = std::make_unique<Doc>();
        d->root = yaml::parseYaml(text, dl);
        d->file = file;
        if (dl.hasErrors()) {
            for (const auto& x : dl.items())
                if (x.severity == yaml::Severity::kError) {
                    fail(file, x.span.begin_line, "YAML: " + x.message);
                    return nullptr;
                }
        }
        Doc* p = d.get();
        docs_.push_back(std::move(d));
        return p;
    }

    bool readFile(const std::string& path, std::string& out) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[8192];
        size_t n;
        out.clear();
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
        return true;
    }

    const YamlNode* findDoc(const std::string& name, std::string& file) {
        auto it = lib_.find(name);
        if (it != lib_.end()) { file = it->second.second; return it->second.first; }
        auto c = cache_.find(name);
        if (c != cache_.end()) { file = c->second->file; return &c->second->root; }
        std::string text;
        if (!opt_.search_dir.empty()) {
            for (const std::string& path : {opt_.search_dir + "/" + name + ".yaml", opt_.search_dir + "/macros/" + name + ".yaml"}) {
                if (readFile(path, text)) {
                    Doc* d = parseDoc(text, path);
                    if (!d) return nullptr;
                    cache_[name] = d;
                    file = path;
                    return &d->root;
                }
            }
        }
        for (int i = 0; i < kBuiltinMacroCount; ++i) {
            if (name == kBuiltinMacros[i][0]) {
                Doc* d = parseDoc(kBuiltinMacros[i][1], std::string("builtin:") + name);
                if (!d) return nullptr;
                cache_[name] = d;
                file = d->file;
                return &d->root;
            }
        }
        return nullptr;
    }

    bool checkSchema(const YamlNode& d, const std::string& file) {
        const YamlNode* s = d.find("schema");
        if (!s || s->scalar != "fluent.sound/v1alpha1")
            return err(file, d.span.begin_line, "schema: fluent.sound/v1alpha1 is required");
        return true;
    }
    bool allowKeys(const YamlNode& d, const std::string& file, std::initializer_list<const char*> keys) {
        for (const auto& e : d.entries) {
            bool ok = false;
            for (const char* k : keys) ok = ok || e.key == k;
            if (!ok) {
                std::string list;
                for (const char* k : keys) list += std::string(list.empty() ? "" : ", ") + k;
                return err(file, e.key_span.begin_line, "unknown key '" + e.key + "' (allowed: " + list + ")");
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- extends → one flat description
    struct Entry {
        const YamlNode* node;
        std::string file;
    };
    struct Flat {
        std::vector<Entry> chain, master;
        bool has_master = false;
        int base_count = 0;
        bool has_parent = false;
        std::vector<std::pair<std::string, std::vector<Entry>>> buses;
        std::vector<std::pair<std::string, Entry>> params;
        std::string io_in = "mono";
        double trim_db = 0;
    };

    bool flatten(const YamlNode& d, const std::string& file, int depth, Flat& out) {
        if (!d.isMapping()) return err(file, 1, "a Sound document must be a mapping");
        if (!checkSchema(d, file)) return false;
        if (d.find("macro") || d.find("bank")) return err(file, d.span.begin_line, "expected a Sound (not a macro / bank)");
        if (!allowKeys(d, file, {"schema", "name", "extends", "io", "params", "chain", "buses", "master", "analysis",
                                 "trim_db", "help", "library", "budget"}))
            return false;
        if (const YamlNode* ex = d.find("extends")) {
            if (depth >= kMaxDepth) return err(file, ex->span.begin_line, "extends: deeper than 2 levels (or a cycle)");
            std::string pf;
            const YamlNode* pd = findDoc(ex->scalar, pf);
            if (!pd) return err(file, ex->span.begin_line, "extends: '" + ex->scalar + "' not found");
            if (!flatten(*pd, pf, depth + 1, out)) return false;
            out.base_count = out.has_parent ? out.base_count : static_cast<int>(out.chain.size());
            out.has_parent = true;
            out.trim_db = 0;
        }
        if (const YamlNode* io = d.find("io")) {
            if (const YamlNode* i = io->find("in")) {
                if (i->scalar != "mono" && i->scalar != "stereo") return err(file, i->span.begin_line, "io.in: mono | stereo");
                if (out.has_parent && out.io_in == "stereo" && i->scalar == "mono") return err(file, i->span.begin_line, "io.in cannot narrow stereo to mono");
                out.io_in = i->scalar;
            }
        }
        if (const YamlNode* ps = d.find("params")) {
            if (!ps->isMapping()) return err(file, ps->span.begin_line, "params: is a mapping");
            for (const auto& e : ps->entries) {
                auto it = std::find_if(out.params.begin(), out.params.end(), [&](auto& p) { return p.first == e.key; });
                if (it != out.params.end()) it->second = {&e.value, file};
                else out.params.push_back({e.key, {&e.value, file}});
            }
        }
        if (const YamlNode* ch = d.find("chain")) {
            if (!ch->isSequence()) return err(file, ch->span.begin_line, "chain: is a list");
            for (const auto& e : ch->elements) out.chain.push_back({&e, file});
        }
        if (const YamlNode* bs = d.find("buses")) {
            if (!bs->isMapping()) return err(file, bs->span.begin_line, "buses: { name: [ nodes ] }");
            for (const auto& e : bs->entries) {
                if (!e.value.isSequence()) return err(file, e.key_span.begin_line, "bus '" + e.key + "' must be a list of nodes");
                std::vector<Entry> list;
                for (const auto& n : e.value.elements) list.push_back({&n, file});
                auto it = std::find_if(out.buses.begin(), out.buses.end(), [&](auto& b) { return b.first == e.key; });
                if (it != out.buses.end()) it->second = list;
                else out.buses.push_back({e.key, list});
            }
        }
        if (const YamlNode* m = d.find("master")) {
            if (!m->isSequence()) return err(file, m->span.begin_line, "master: is a list");
            out.master.clear();
            for (const auto& e : m->elements) out.master.push_back({&e, file});
            out.has_master = true;
        }
        if (const YamlNode* t = d.find("trim_db")) {
            if (!parseNumber(t->scalar, out.trim_db) || out.trim_db < -24 || out.trim_db > 24)
                return err(file, t->span.begin_line, "trim_db: -24..24");
        }
        return true;
    }

    // ---------------------------------------------------------------- values
    bool evalExpr(const std::string& s, size_t& i, const Ctx& ctx, Val& out, const std::string& file, uint32_t line);
    bool evalTerm(const std::string& s, size_t& i, const Ctx& ctx, Val& out, const std::string& file, uint32_t line);
    bool evalFactor(const std::string& s, size_t& i, const Ctx& ctx, Val& out, const std::string& file, uint32_t line);

    bool eval(const YamlNode& n, const Ctx& ctx, Val& out, const std::string& file) {
        out = Val{};
        if (n.isSequence() || n.isMapping()) return err(file, n.span.begin_line, "expected a scalar value");
        const std::string& s = n.scalar;
        if (n.isPlainScalar() && (s.find('$') != std::string::npos)) {
            size_t i = 0;
            if (!evalExpr(s, i, ctx, out, file, n.span.begin_line)) return false;
            while (i < s.size() && s[i] == ' ') ++i;
            if (i != s.size()) return err(file, n.span.begin_line, "cannot read expression '" + s + "'");
            return true;
        }
        double v;
        if (n.isPlainScalar() && parseNumber(s, v)) { out.num = v; return true; }
        if (s == "true") { out.num = 1; return true; }
        if (s == "false") { out.num = 0; return true; }
        out.is_text = true;
        out.text = s;
        return true;
    }

    static Val scale(Val a, double k) {
        a.num *= k;
        a.k *= k;
        for (auto& o : a.origins) { o.k *= k; o.c *= k; }
        return a;
    }
    static Val shift(Val a, double c) {
        a.num += c;
        for (auto& o : a.origins) o.c += c;
        return a;
    }
    bool isConst(const Val& v) const { return !v.bound() && v.origins.empty(); }

    // ---------------------------------------------------------------- building
    Preset* cur_ = nullptr;
    std::string preset_name_;
    std::map<std::string, GraphParam*> gp_by_name_;
    struct PendingSend {
        Node* node;
        std::string bus;
        int channels;
        std::string file;
        uint32_t line;
    };
    std::vector<PendingSend> sends_;

    int handle(const std::string& name, float value, float mn, float mx, bool runtime) {
        auto it = handle_idx_.find(name);
        if (it != handle_idx_.end()) return it->second;
        ParamHandle h;
        h.name = name;
        h.value = value;
        h.min = mn;
        h.max = mx;
        h.runtime = runtime;
        eng_->handles_.push_back(h);
        handle_idx_[name] = static_cast<int>(eng_->handles_.size()) - 1;
        return handle_idx_[name];
    }

    bool assign(Node& node, int pi, int item, const Val& v, const std::string& file, uint32_t line, const std::string& hname) {
        const ParamSpec& ps = node.info()->params[pi];
        const int slot = node.slotOf(pi, item);
        double value = v.num;
        if (v.is_text) {
            if (!ps.choices) return err(file, line, std::string(ps.name) + ": expected a number, got '" + v.text + "'");
            int idx = 0;
            bool found = false;
            const char* c = ps.choices;
            while (*c) {
                const char* e = std::strchr(c, '|');
                const size_t len = e ? static_cast<size_t>(e - c) : std::strlen(c);
                if (v.text.size() == len && v.text.compare(0, len, c, len) == 0) { found = true; break; }
                ++idx;
                c += len + (e ? 1 : 0);
                if (!e) break;
            }
            if (!found) return err(file, line, std::string(ps.name) + ": '" + v.text + "' is not one of " + ps.choices);
            value = idx;
        }
        if (v.bound()) {
            if (ps.smooth == Smooth::kFixed)
                return err(file, line, std::string(ps.name) + " is fixed at compile time; it cannot follow $params." + v.gparam);
            GraphParam* gp = gp_by_name_[v.gparam];
            value = v.k * gp->cur + v.num;
            cur_->bindings.push_back({gp, &node, slot, v.k, v.num});
        } else if (value < ps.min - 1e-6 || value > ps.max + 1e-6) {
            return err(file, line, std::string(ps.name) + ": " + fmt(value) + " is out of range [" + fmt(ps.min) + ", " +
                                       fmt(ps.max) + "]" + (ps.unit && *ps.unit ? std::string(" ") + ps.unit : ""));
        }
        node.setInitial(slot, static_cast<float>(value));
        const int h = handle(hname, static_cast<float>(value), ps.min, ps.max, ps.smooth != Smooth::kFixed);
        eng_->handles_[h].targets.push_back({nullptr, &node, slot, 1, 0});
        for (const auto& o : v.origins) {
            auto it = handle_idx_.find(o.handle);
            if (it != handle_idx_.end() && ps.smooth != Smooth::kFixed)
                eng_->handles_[it->second].targets.push_back({nullptr, &node, slot, o.k, o.c});
        }
        if (v.bound()) node_binds_.push_back(hname + "@" + fmt(v.k) + "*" + v.gparam + "+" + fmt(v.num));
        return true;
    }

    bool addEntry(Preset::Section& sec, int& ch, const YamlNode& item, const std::string& file, const Ctx& ctx,
                  const std::string& prefix, Counters& cnt, int mdepth, Where where) {
        const uint32_t line = item.span.begin_line;
        if (!item.isMapping() || item.entries.size() != 1)
            return err(file, line, "each entry is one '- <node>: { params }'");
        const std::string& type = item.entries[0].key;
        const YamlNode& args = item.entries[0].value;
        if (!args.isNull() && !args.isMapping()) return err(file, line, type + ": params must be a mapping");
        std::string id;
        if (const YamlNode* idn = args.find("id")) id = idn->scalar;
        else id = type + std::to_string(++cnt[type]);
        const std::string full = prefix + id;
        const std::string hprefix = preset_name_ + "." + full + ".";

        if (const NodeInfo* info = findNode(type)) {
            if (type == "send" && where != Where::kChain) return err(file, line, "send: only in chain (bus → bus sends are v2)");
            if (type == "mix" && where != Where::kMaster) return err(file, line, "mix: only in master (it mixes against the dry tap)");
            if (++node_count_ > kMaxNodes) return err(file, line, "more than 64 nodes in one preset");
            std::unique_ptr<Node> node(info->create());
            // grouped list (e.g. bands)
            const char* group = nullptr;
            for (int i = 0; i < info->nparams; ++i)
                if (info->params[i].group) group = info->params[i].group;
            int items = 1;
            const YamlNode* glist = group ? args.find(group) : nullptr;
            if (glist) {
                if (!glist->isSequence()) return err(file, glist->span.begin_line, std::string(group) + ": is a list of maps");
                items = static_cast<int>(glist->elements.size());
                if (items < 1 || items > info->max_items)
                    return err(file, glist->span.begin_line, std::string(group) + ": 1.." + std::to_string(info->max_items) + " entries");
            }
            node->init(info, items, opt_.sample_rate);
            node->id = full;
            canon_ += "\n" + preset_name_ + "." + full + ":" + type;
            for (const auto& e : args.entries) {
                if (e.key == "id") continue;
                if (type == "send" && e.key == "to") continue;
                if (group && e.key == group) continue;
                int pi = -1;
                for (int i = 0; i < info->nparams; ++i)
                    if (!info->params[i].group && e.key == info->params[i].name) pi = i;
                if (pi < 0) {
                    std::string list;
                    for (int i = 0; i < info->nparams; ++i)
                        if (!info->params[i].group) list += std::string(list.empty() ? "" : ", ") + info->params[i].name;
                    if (group) list += std::string(", ") + group;
                    return err(file, e.key_span.begin_line, type + ": unknown param '" + e.key + "' (has: " + list + ")");
                }
                Val v;
                if (!eval(e.value, ctx, v, file)) return false;
                if (!assign(*node, pi, 0, v, file, e.value.span.begin_line, hprefix + e.key)) return false;
            }
            // unset params still get handles (so every param is addressable by name)
            for (int i = 0; i < info->nparams; ++i) {
                if (info->params[i].group) continue;
                const std::string hn = hprefix + info->params[i].name;
                if (!handle_idx_.count(hn)) {
                    Val v;
                    v.num = info->params[i].def;
                    if (!assign(*node, i, 0, v, file, line, hn)) return false;
                }
            }
            if (glist) {
                for (int k = 0; k < items; ++k) {
                    const YamlNode& m = glist->elements[k];
                    if (!m.isMapping()) return err(file, m.span.begin_line, std::string(group) + ": entries are maps");
                    for (const auto& e : m.entries) {
                        int pi = -1;
                        for (int i = 0; i < info->nparams; ++i)
                            if (info->params[i].group && e.key == info->params[i].name) pi = i;
                        if (pi < 0) return err(file, e.key_span.begin_line, std::string(group) + ": unknown key '" + e.key + "'");
                        Val v;
                        if (!eval(e.value, ctx, v, file)) return false;
                        if (!assign(*node, pi, k, v, file, e.value.span.begin_line,
                                    hprefix + group + "." + std::to_string(k) + "." + e.key))
                            return false;
                    }
                    for (int i = 0; i < info->nparams; ++i) {   // every band param gets a name too
                        if (!info->params[i].group) continue;
                        const std::string hn = hprefix + group + "." + std::to_string(k) + "." + info->params[i].name;
                        if (handle_idx_.count(hn)) continue;
                        Val v;
                        v.num = info->params[i].def;
                        if (!assign(*node, i, k, v, file, m.span.begin_line, hn)) return false;
                    }
                }
            }
            if (type == "send") {
                const YamlNode* to = args.find("to");
                if (!to || !to->isScalar()) return err(file, line, "send: needs to: <bus>");
                sends_.push_back({node.get(), to->scalar, ch, file, line});
            }
            // canonical form for the digest: slot values in table order, bindings sorted
            for (int k = 0; k < node->slotCount(); ++k) canon_ += " " + fmt(node->slot(k).cur);
            std::sort(node_binds_.begin(), node_binds_.end());
            for (const auto& b : node_binds_) canon_ += " " + b;
            node_binds_.clear();
            std::string perr;
            Prepare pr{opt_.sample_rate, ch, eng_->arena_, perr};
            node->prepare(pr);
            if (!perr.empty()) return err(file, line, type + ": " + perr);
            sec.ch_in.push_back(ch);
            ch = info->out_channels(ch);
            cur_->cost += info->cost;
            sec.nodes.push_back(std::move(node));
            return true;
        }

        // a macro
        std::string mfile;
        const YamlNode* md = findDoc(type, mfile);
        if (!md || !md->find("macro")) {
            std::string nodes;
            for (const NodeInfo* i : nodeRegistry()) nodes += std::string(nodes.empty() ? "" : ", ") + i->name;
            return err(file, line, "unknown node '" + type + "' (nodes: " + nodes + "; or a macro file " + type + ".yaml)");
        }
        if (mdepth >= kMaxMacroDepth) return err(file, line, "macros nested deeper than 4");
        if (!allowKeys(*md, mfile, {"schema", "macro", "help", "io", "params", "chain"})) return false;
        Ctx inner;
        inner.parent = &ctx;
        std::set<std::string> declared;
        if (const YamlNode* ps = md->find("params")) {
            for (const auto& e : ps->entries) {
                declared.insert(e.key);
                double mn = -1e9, mx = 1e9, def = 0;
                if (const YamlNode* r = e.value.find("range"))
                    if (r->isSequence() && r->elements.size() == 2) { parseNumber(r->elements[0].scalar, mn); parseNumber(r->elements[1].scalar, mx); }
                const YamlNode* dn = e.value.find("default");
                if (!dn) return err(mfile, e.key_span.begin_line, "macro param '" + e.key + "' needs a default");
                Val v;
                const YamlNode* given = args.find(e.key);
                if (given) {
                    if (!eval(*given, ctx, v, file)) return false;
                } else {
                    if (!eval(*dn, ctx, v, mfile)) return false;
                }
                if (v.is_text) {
                    inner.vars[e.key] = v;
                    continue;
                }
                parseNumber(dn->scalar, def);
                if (isConst(v) && (v.num < mn - 1e-6 || v.num > mx + 1e-6))
                    return err(file, given ? given->span.begin_line : line,
                               type + "." + e.key + ": " + fmt(v.num) + " is out of range [" + fmt(mn) + ", " + fmt(mx) + "]");
                const std::string hn = hprefix + e.key;
                handle(hn, static_cast<float>(v.bound() ? def : v.num), static_cast<float>(mn), static_cast<float>(mx), true);
                if (!v.bound()) {   // a macro param set at runtime moves the inner params it feeds
                    Val w;
                    w.num = v.num;
                    w.origins = v.origins;
                    w.origins.push_back({hn, 1, 0});
                    v = w;
                }
                inner.vars[e.key] = v;
            }
        }
        for (const auto& e : args.entries)
            if (e.key != "id" && !declared.count(e.key))
                return err(file, e.key_span.begin_line, type + ": unknown param '" + e.key + "'");
        const YamlNode* mc = md->find("chain");
        if (!mc || !mc->isSequence()) return err(mfile, md->span.begin_line, "macro: needs chain: [ ... ]");
        Counters inner_cnt;
        canon_ += "\n" + preset_name_ + "." + full + ":macro " + type;
        for (const auto& it : mc->elements)
            if (!addEntry(sec, ch, it, mfile, inner, full + ".", inner_cnt, mdepth + 1, where)) return false;
        return true;
    }

    bool buildPreset(const std::string& name, const YamlNode* doc, const std::string& file) {
        Flat flat;
        if (!flatten(*doc, file, 0, flat)) return false;
        auto preset = std::make_unique<Preset>();
        Preset& p = *preset;
        p.name = name;
        cur_ = &p;
        preset_name_ = name;
        gp_by_name_.clear();
        sends_.clear();
        node_count_ = 0;
        canon_ += "\npreset " + name;
        p.in_channels = flat.io_in == "stereo" ? 2 : 1;
        p.trim = static_cast<float>(dsp::db2lin(flat.trim_db));
        canon_ += " trim=" + fmt(flat.trim_db);
        if (static_cast<int>(flat.params.size()) > kMaxParams) return err(file, 1, "more than 64 params");

        // $params
        for (const auto& [pname, ent] : flat.params) {
            const YamlNode& d = *ent.node;
            const std::string& f = ent.file;
            if (!d.isMapping()) return err(f, d.span.begin_line, "param '" + pname + "': { type, default, range }");
            auto gp = std::make_unique<GraphParam>();
            gp->name = pname;
            const std::string type = d.find("type") ? d.find("type")->scalar : "f32";
            double mn = 0, mx = 1, def = 0;
            if (type == "f32") {
                const YamlNode* r = d.find("range");
                if (!r || !r->isSequence() || r->elements.size() != 2 || !parseNumber(r->elements[0].scalar, mn) ||
                    !parseNumber(r->elements[1].scalar, mx) || !(mn < mx))
                    return err(f, d.span.begin_line, "param '" + pname + "': range: [min, max] is required");
                const YamlNode* dn = d.find("default");
                if (!dn || !parseNumber(dn->scalar, def)) return err(f, d.span.begin_line, "param '" + pname + "': default: <number>");
            } else if (type == "bool") {
                const YamlNode* dn = d.find("default");
                def = dn && dn->scalar == "true" ? 1 : 0;
            } else if (type == "enum") {
                const YamlNode* vs = d.find("values");
                if (!vs || !vs->isSequence() || vs->elements.empty()) return err(f, d.span.begin_line, "enum param needs values: [...]");
                mx = static_cast<double>(vs->elements.size() - 1);
                const YamlNode* dn = d.find("default");
                for (size_t k = 0; dn && k < vs->elements.size(); ++k)
                    if (vs->elements[k].scalar == dn->scalar) def = static_cast<double>(k);
            } else {
                return err(f, d.span.begin_line, "param type: f32 | bool | enum");
            }
            if (def < mn || def > mx) return err(f, d.span.begin_line, "param '" + pname + "': default out of range");
            gp->min = static_cast<float>(mn);
            gp->max = static_cast<float>(mx);
            gp->def = gp->cur = gp->from = gp->to = static_cast<float>(def);
            gp->dur = 0.02;
            if (const YamlNode* an = d.find("animate")) {
                double dur = 0.02;
                if (const YamlNode* du = an->find("duration")) parseNumber(du->scalar, dur);
                gp->dur = std::max(0.02, std::min(60.0, dur));
                if (const YamlNode* ea = an->find("ease")) {
                    const std::string& e = ea->scalar;
                    gp->ease_kind = e == "linear" ? 0 : e == "ease_in" ? 1 : e == "ease_out" ? 2 : e == "ease_in_out" ? 3 : -1;
                    if (gp->ease_kind < 0) return err(f, ea->span.begin_line, "ease: linear | ease_in | ease_out | ease_in_out");
                }
            }
            gp_by_name_[pname] = gp.get();
            p.params.push_back(gp.get());
            const int h = handle(pname, gp->cur, gp->min, gp->max, true);
            eng_->handles_[h].targets.push_back({gp.get(), nullptr, 0, 1, 0});
            canon_ += " $" + pname + "=" + fmt(def);
            eng_->gparams_.push_back(std::move(gp));
        }

        Ctx ctx;
        Counters cnt;
        int ch = p.in_channels;
        // chain (the dry tap sits after the root parent's part)
        p.dry_tap = 0;
        for (size_t k = 0; k < flat.chain.size(); ++k) {
            const Entry& e = flat.chain[k];
            if (!addEntry(p.chain, ch, *e.node, e.file, ctx, "", cnt, 0, Where::kChain)) return false;
            if (static_cast<int>(k) + 1 == flat.base_count) p.dry_tap = static_cast<int>(p.chain.nodes.size());
        }
        // buses
        if (static_cast<int>(flat.buses.size()) > kMaxBuses) return err(file, 1, "more than 8 buses");
        p.buses.resize(flat.buses.size());
        for (size_t b = 0; b < flat.buses.size(); ++b) {
            Preset::Bus& bus = p.buses[b];
            bus.name = flat.buses[b].first;
            bus.input.channels = 1;
            for (const auto& s : sends_)
                if (s.bus == bus.name) bus.input.channels = std::max(bus.input.channels, s.channels);
            bus.input.ch[0] = eng_->arena_.alloc<float>(kControlRate);
            bus.input.ch[1] = eng_->arena_.alloc<float>(kControlRate);
            int bch = bus.input.channels;
            canon_ += "\n" + name + " bus " + bus.name;
            for (const auto& e : flat.buses[b].second)
                if (!addEntry(bus.sec, bch, *e.node, e.file, ctx, "", cnt, 0, Where::kBus)) return false;
            bus.out_channels = bch;
        }
        for (const auto& s : sends_) {
            auto it = std::find_if(p.buses.begin(), p.buses.end(), [&](const Preset::Bus& b) { return b.name == s.bus; });
            if (it == p.buses.end()) return err(s.file, s.line, "send: no bus named '" + s.bus + "'");
            s.node->connectBus(&it->input);
        }
        // master (inside a bank the presets' own limiters are dropped: one limiter after the bank sum)
        int mch = 2;
        for (const auto& e : flat.master) {
            if (eng_->bank_ && e.node->isMapping() && e.node->entries.size() == 1 && e.node->entries[0].key == "limiter") continue;
            if (!addEntry(p.master, mch, *e.node, e.file, ctx, "", cnt, 0, Where::kMaster)) return false;
        }
        if (p.cost > kBudget)
            return err(file, 1, "preset '" + name + "' costs ~" + std::to_string(p.cost) + " ops/sample (budget 600)");

        Arena& a = eng_->arena_;
        p.buf[0] = a.alloc<float>(kControlRate);
        p.buf[1] = a.alloc<float>(kControlRate);
        p.dry = a.alloc<float>(kControlRate);
        p.gin = a.alloc<float>(kControlRate);
        p.gout = a.alloc<float>(kControlRate);
        eng_->presets_.push_back(std::move(preset));
        cur_ = nullptr;
        return true;
    }

    void setupAnalysis() {
        Engine& e = *eng_;
        Arena& a = e.arena_;
        const double sr = e.sr_, tr = sr / kControlRate;
        e.a10_ = static_cast<float>(dsp::poleMs(10.0, sr));
        e.ka_ = static_cast<float>(dsp::poleMs(5.0, tr));
        e.kr_ = static_cast<float>(dsp::poleMs(120.0, tr));
        e.peak_decay_ = static_cast<float>(dsp::poleMs(300.0, tr));
        e.onset_decay_ = static_cast<float>(dsp::poleMs(50.0, tr));
        e.refractory_ticks_ = static_cast<int>(0.060 * tr + 0.5);
        e.hold_ticks_ = static_cast<int>(0.100 * tr + 0.5);
        e.fft_ring_ = a.alloc<float>(256);
        e.fft_re_ = a.alloc<float>(256);
        e.fft_im_ = a.alloc<float>(256);
        e.fft_win_ = a.alloc<float>(256);
        e.fft_cos_ = a.alloc<float>(128);
        e.fft_sin_ = a.alloc<float>(128);
        for (int i = 0; i < 256; ++i) e.fft_win_[i] = static_cast<float>(0.5 - 0.5 * dsp::dcos(2.0 * dsp::kPi * i / 256.0));
        for (int i = 0; i < 128; ++i) {
            double s, c;
            dsp::dsincos(-2.0 * dsp::kPi * i / 256.0, s, c);
            e.fft_cos_[i] = static_cast<float>(c);
            e.fft_sin_[i] = static_cast<float>(s);
        }
    }

    const Options& opt_;
    std::string& err_;
    std::unique_ptr<Engine> eng_;
    std::vector<std::unique_ptr<Doc>> docs_;
    std::map<std::string, std::pair<const YamlNode*, std::string>> lib_;
    std::map<std::string, Doc*> cache_;
    std::map<std::string, int> handle_idx_;
    std::string canon_;
    std::vector<std::string> node_binds_;
    int node_count_ = 0;
};

// ---------------------------------------------------------------- expressions: $x, $params.x, numbers, + - * / ( )
bool Compiler::evalFactor(const std::string& s, size_t& i, const Ctx& ctx, Val& out, const std::string& file, uint32_t line) {
    while (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) return err(file, line, "expression ends early: '" + s + "'");
    if (s[i] == '-') {
        ++i;
        if (!evalFactor(s, i, ctx, out, file, line)) return false;
        out = scale(out, -1);
        return true;
    }
    if (s[i] == '(') {
        ++i;
        if (!evalExpr(s, i, ctx, out, file, line)) return false;
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size() || s[i] != ')') return err(file, line, "missing ')' in '" + s + "'");
        ++i;
        return true;
    }
    if (s[i] == '$') {
        size_t j = ++i;
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.')) ++j;
        const std::string name = s.substr(i, j - i);
        i = j;
        out = Val{};
        if (name.compare(0, 7, "params.") == 0) {
            const std::string pn = name.substr(7);
            if (!gp_by_name_.count(pn)) return err(file, line, "$params." + pn + " is not declared in params:");
            out.gparam = pn;
            out.k = 1;
            return true;
        }
        for (const Ctx* c = &ctx; c; c = c->parent) {
            auto it = c->vars.find(name);
            if (it != c->vars.end()) { out = it->second; return true; }
        }
        return err(file, line, "unknown $" + name + " (macro params are $name, sound params are $params.name)");
    }
    size_t j = i;
    while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' ||
                            ((s[j] == '-' || s[j] == '+') && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
        ++j;
    double v;
    if (j == i || !parseNumber(s.substr(i, j - i), v)) return err(file, line, "cannot read '" + s + "'");
    i = j;
    out = Val{};
    out.num = v;
    return true;
}

bool Compiler::evalTerm(const std::string& s, size_t& i, const Ctx& ctx, Val& out, const std::string& file, uint32_t line) {
    if (!evalFactor(s, i, ctx, out, file, line)) return false;
    for (;;) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size() || (s[i] != '*' && s[i] != '/')) return true;
        const char op = s[i++];
        Val r;
        if (!evalFactor(s, i, ctx, r, file, line)) return false;
        if (op == '*') {
            if (!r.bound() && r.origins.empty()) out = scale(out, r.num);
            else if (!out.bound() && out.origins.empty()) out = scale(r, out.num);
            else if (!out.bound() && !r.bound()) { const double v = out.num * r.num; out = Val{}; out.num = v; }
            else return err(file, line, "'" + s + "': only one side of * may be a runtime param");
        } else {
            if (r.bound() || r.num == 0) return err(file, line, "'" + s + "': divide by a constant only");
            out = scale(out, 1.0 / r.num);
        }
    }
}

bool Compiler::evalExpr(const std::string& s, size_t& i, const Ctx& ctx, Val& out, const std::string& file, uint32_t line) {
    if (!evalTerm(s, i, ctx, out, file, line)) return false;
    for (;;) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size() || (s[i] != '+' && s[i] != '-')) return true;
        const char op = s[i++];
        Val r;
        if (!evalTerm(s, i, ctx, r, file, line)) return false;
        if (op == '-') r = scale(r, -1);
        if (out.bound() && r.bound()) return err(file, line, "'" + s + "': only one runtime param per expression");
        if (r.bound() || (!r.origins.empty() && out.origins.empty() && !out.bound())) std::swap(out, r);
        if (!r.origins.empty()) out.origins.clear();   // two macro params: no single runtime handle feeds it
        out = shift(out, r.num);
    }
}

std::unique_ptr<Engine> Engine::create(const std::string& yaml, const Options& opt, std::string& error) {
    error.clear();
    Compiler c(opt, error);
    auto e = c.build(yaml);
    if (!e && error.empty()) error = "compile failed";
    return e;
}

}  // namespace fluent_audio
