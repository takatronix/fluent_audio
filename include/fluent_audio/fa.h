/* fluent_audio — C API (design §5.3). The same functions are exported from libfluent_audio.so, from
 * fluent_audio.wasm (the AudioWorklet calls them) and are wrapped by python/fluent_audio.py.
 *
 *   FaInstance* fa = fa_create(yaml, 48000, 128);      // NULL → fa_error()
 *   float* in = fa_in_buffer(fa, 0);                    // write mono voice here (max_block samples)
 *   fa_process(fa, n);                                  // RT safe
 *   float* l = fa_out_buffer(fa, 0), *r = fa_out_buffer(fa, 1);
 *   fa_select(fa, "fluent");  fa_set_param_f32(fa, "amount", 0.5f);
 *
 * Strings are UTF-8. Names: a $param ("amount"), or a node param "<preset>.<node>.<param>"
 * (e.g. "kepler.reverb1.decay_s", "fluent.shimmer1.semitones", "kepler.eq2.bands.0.gain_db");
 * fa_param_count / fa_param_name list them all. Every function is safe on a NULL instance (returns 0 / NULL).
 */
#ifndef FLUENT_AUDIO_FA_H
#define FLUENT_AUDIO_FA_H

#include <stdint.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define FA_API EMSCRIPTEN_KEEPALIVE
#elif defined(_WIN32)
#define FA_API __declspec(dllexport)
#else
#define FA_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FaInstance FaInstance;

typedef struct FaAnalysis {
    uint32_t version, size;
    uint64_t frame;
    float voice, level, peak, onset, centroid, tail;
} FaAnalysis;

/* lifecycle */
FA_API FaInstance* fa_create(const char* yaml_utf8, float sample_rate, int max_block);
/* like fa_create, but `extends:` / bank presets / macros are also looked up in search_dir */
FA_API FaInstance* fa_create_ex(const char* yaml_utf8, float sample_rate, int max_block, const char* search_dir);
FA_API const char* fa_error(void);
FA_API void fa_destroy(FaInstance*);
FA_API const char* fa_digest(FaInstance*);
FA_API int fa_latency_samples(FaInstance*);
FA_API int fa_memory_bytes(FaInstance*);

/* audio: non-interleaved float32, max_block long */
FA_API float* fa_in_buffer(FaInstance*, int channel);
FA_API float* fa_out_buffer(FaInstance*, int channel);
FA_API int fa_in_channels(FaInstance*);
FA_API int fa_process(FaInstance*, int frames); /* RT safe. 1 = ok */

/* params (applied at the next control tick, or at `frame`) */
FA_API int fa_set_param_f32(FaInstance*, const char* name, float v);
FA_API int fa_set_param_bool(FaInstance*, const char* name, int v);
FA_API float fa_get_param_f32(FaInstance*, const char* name);
FA_API int fa_param_index(FaInstance*, const char* name);
FA_API int fa_param_count(FaInstance*);
FA_API const char* fa_param_name(FaInstance*, int index);
FA_API float fa_param_min(FaInstance*, int index);
FA_API float fa_param_max(FaInstance*, int index);
FA_API int fa_set_param_idx(FaInstance*, int index, float v);                      /* RT safe */
FA_API int fa_set_param_at(FaInstance*, int index, float v, uint64_t frame);      /* deterministic */
FA_API int fa_set_param_at_f64(FaInstance*, int index, float v, double frame);   /* same, for JS */

/* presets (a bank: crossfade; old tails ring out) */
FA_API int fa_select(FaInstance*, const char* preset);
FA_API int fa_preset_index(FaInstance*, const char* preset); /* aliases resolve */
FA_API int fa_preset_count(FaInstance*);
FA_API const char* fa_preset_name(FaInstance*, int index);
FA_API int fa_alias_count(FaInstance*);                                            /* look → preset aliases */
FA_API const char* fa_alias_name(FaInstance*, int index);
FA_API int fa_alias_preset(FaInstance*, int index);
FA_API int fa_select_idx(FaInstance*, int index);                                  /* RT safe */
FA_API int fa_select_at(FaInstance*, int index, uint64_t frame);
FA_API int fa_current_preset(FaInstance*);
FA_API int fa_bypass(FaInstance*, int on); /* whole graph out (20 ms fade), tails ring out */
FA_API void fa_reset(FaInstance*);

/* analysis + diagnostics */
FA_API const FaAnalysis* fa_analysis(FaInstance*);
FA_API int fa_diagnostics_count(FaInstance*);
FA_API const char* fa_diagnostics(FaInstance*); /* why the last setter returned 0 (then cleared) */
FA_API const char* fa_drain_events(FaInstance*);
FA_API double fa_frame(FaInstance*);

/* introspection: every registered node type and its param table, as JSON (for UIs, docs, bindings) */
FA_API const char* fa_nodes_json(void);
FA_API const char* fa_version(void);

#ifdef __cplusplus
}
#endif

#endif
