# フィルタの足し方

fluent_vision_ros2 でノードのパッケージを 1 つ置くのと同じ感覚で、**1 つのまとまりを置くだけ**で使えるようにしてあります。
足し方は 3 通り。上から順に簡単で、**たいていは 1 か 2 で足ります**。

| やりたいこと | 方法 | 触るファイル |
|---|---|---|
| ある look の音を変える | プリセットの YAML を書き換える | `presets/<look>.yaml` だけ |
| 今ある部品の組み合わせに名前を付けて、何度も使う | **YAML マクロ** | `macros/<name>.yaml` を 1 枚置く |
| まったく新しい信号処理 | **C++ ノード** | `src/nodes/<name>.cpp` を 1 枚置く（雛形あり） |

どの方法で足しても、YAML ローダ・C API（名前で set / get）・wasm / JS ラッパ・Python・ドキュメント（`docs/nodes.md`）・
テストが**自動で**それを知ります。ほかのファイルの書き換え・登録の一覧はありません（ビルドは `src/nodes/` と `macros/` を glob で拾う）。

---

## 1. プリセットを書き換える（YAML だけ）

```yaml
# presets/kepler.yaml
chain:
  - eq: { bands: [ { type: peak, f: 3200, q: 1.0, gain_db: 1.5 } ] }
  - exciter: { freq: 4500, drive: 2.0, mix: 0.08 }
  - send: { to: room, gain: 0.05 }
buses:
  room:
    - reverb: { decay_s: 0.5, size: 0.45, damp_hz: 7000, predelay_ms: 6 }
```

- 使えるノードと param の一覧・範囲は `docs/nodes.md`（`build/fa_render --list --markdown` で再生成）。
- 範囲外・知らない param・知らないノードは**読み込み時に行番号付きで拒否**されます（`<file>:12: eq: unknown param 'gain' (has: bands)`）。
- 聞いてみる: `build/fa_render presets/looks.yaml kepler in.wav out.wav`。音量をそろえ直すなら
  `build/fa_render --loudness out.wav` を見て `trim_db:` を直す。

## 2. YAML マクロ（C++ なしで新しい「フィルタ」）

**名前の付いた小さな鎖**です。自分の param を持ち、それを中のノードの param に `$名前` で渡します。
使う側からは普通のノードと同じに見えます。例: 本物の `macros/radio.yaml`

```yaml
schema: fluent.sound/v1alpha1
macro: radio
help: Band-limited radio colour (HP + LP + presence) with gentle ADAA saturation.
params:                                   # 外に見せる param（default と range は必須）
  low_hz:      { default: 220,  range: [40, 1000],   unit: Hz }
  high_hz:     { default: 6000, range: [2000, 16000], unit: Hz }
  presence_db: { default: 3,    range: [-12, 12],    unit: dB }
  drive:       { default: 1.8,  range: [1, 10] }
  mix:         { default: 0.6,  range: [0, 1] }
chain:
  - eq: { bands: [ { type: hp, f: $low_hz, q: 0.8 }, { type: lp, f: $high_hz, q: 0.8 },
                   { type: peak, f: 1400, q: 0.9, gain_db: $presence_db } ] }
  - saturate: { drive: $drive, mix: $mix }
```

使い方: `- radio: { low_hz: 300, mix: 0.4 }`。

- 式も書けます: `depth_ms: $wobble * 6`、`f: $hp_hz + 100`（定数との + − × ÷ と括弧）。
  `$params.amount` のような**実行中に動く param** を渡すこともでき、その時も式はそのまま効きます。
- マクロの param は**実行中に名前で動かせます**: `fa_set_param_f32(fa, "fluent.shimmer1.semitones", 7)`
  （中の pitch_shift に届く）。名前は `<プリセット>.<マクロ名><番号>.<param>`、または `id:` で付けた名前。
- 置き場所: `macros/`（ライブラリに埋め込まれ、どこでも使える）か、プリセットの隣の `presets/macros/`（その束だけ）。
- 今あるマクロ: `radio`, `shimmer`（hp → pitch_shift → reverb）, `doubler`（chorus の 1 声・ランダム揺れ）。

## 3. C++ ノード（新しい信号処理）

```sh
python3 tools/fa_new_node.py tremolo      # src/nodes/tremolo.cpp と tests/cases/tremolo.yaml を作る
cmake --build build && build/fa_tests nodes   # どのノードにも自動で付くテスト（下）
build/fa_tests golden --update            # golden WAV を作る（中身を確かめてからコミット）
```

1 ファイル = 1 ノード。**param の表**と、`control()`（32 サンプルごと）・`process()`（音）を書いて、最後の 1 行で登録します。
これがほぼ雛形そのもの（30 行）:

```cpp
// tremolo — 音量をゆっくり揺らす。
#include "fluent_audio/node.hpp"
namespace fluent_audio {

class Tremolo final : public Node {
public:
    enum { kDepth, kRate };
    static constexpr ParamSpec kParams[] = {
        // name      unit  min   max  既定  補間             説明
        {"depth",   "",   0,    1,   0.2f, Smooth::kLinear, "how deep"},
        {"rate_hz", "Hz", 0.1f, 20,  4,    Smooth::kLog,    "wobble speed"},
    };
    void reset() noexcept override { phase_ = 0; depth_.reset(); }
    void control() noexcept override {                 // 32 サンプルごと。param はもう補間済み
        inc_ = static_cast<float>(p(kRate) / sampleRate());
        depth_.next(p(kDepth));                        // サンプル単位で直線補間する値
    }
    void process(Slice& s) noexcept override {         // s.n ≤ 32。確保・ロック・I/O は禁止
        for (int i = 0; i < s.n; ++i) {
            const float g = 1 - depth_.at(s.pos + i) * (0.5f + 0.5f * dsp::sinTurns(phase_));
            if ((phase_ += inc_) >= 1) phase_ -= 1;
            for (int c = 0; c < s.channels; ++c) s.ch[c][i] *= g;
        }
    }
private:
    float phase_ = 0, inc_ = 0;
    dsp::Ramp depth_;
};
FA_NODE(tremolo, Tremolo, "Slow amplitude wobble.")   // ← YAML の名前 `- tremolo: {...}`

}  // namespace fluent_audio
```

### param の表（`ParamSpec`）
- `Smooth::kLinear` / `kLog`: 実行中に動かせる。基底クラスが**最低 20 ms で補間**してから `control()` を呼ぶ（クリックが出ない）。
  `kLog` は周波数・Q 向き（対数で滑らせる）。
- `Smooth::kFixed`: コンパイル時だけ（種類・声の数・バッファの大きさ）。`$params` に繋ごうとすると読み込みで拒否。
- `choices = "hp|lp|peak"` で列挙（YAML では名前で書ける）、`group = "bands"` でリスト（`bands: [{...}, {...}]`、最大 `kMaxItems` 個）。
- `range` は**バッファの確保量の根拠**です（遅延線の長さは最大値で `prepare()` 時に確保する）。

### リアルタイムの規則（基底クラスとテストが守らせる）
- メモリは **`prepare(Prepare& p)` の `p.arena.alloc<float>(n)` だけ**。コンパイルが終わると arena は封印され、後から頼んでも nullptr。
- `control()` / `process()` / `reset()` は `noexcept`。確保・ロック・システムコール・ログ・文字列は禁止。
- 数学は `dsp::`（`sinTurns`, `fexp`, `ftanh`, `dexp`, `dlog`, `dsincos`, `db2lin`, `rbj` …）を使う。`std::sin` 等の libm は
  **使わない**（native と wasm でビットが変わる）。帰還のある状態には `dsp::flush()`（2⁻¹⁰⁰ 未満を 0 に）。
- 出力に NaN / Inf が出たら、グラフがそのノードを reset して数える（声が黙りっぱなしにならない）。
- mono → stereo に広げるノードは `static int outChannels(int) { return 2; }`。

### 何もしなくても付くテスト（`tests/test_nodes.cpp`）
登録された**全ノード**について: 既定値・全 param 最小・全 param 最大で読み込める／出力が有限／**process 中の確保 0 回**
（malloc / new をフック）／1〜512 のランダムなチャンクに切っても**ビット一致**／入力が止まると尾が**ちょうど 0** で終わる。
加えて golden（既定値のノード単体 `node_<name>` と雛形の `tests/cases/<name>.yaml`）と、native ↔ wasm のビット一致。

### 自分で確かめる
```sh
build/fa_render --list | grep -A3 tremolo           # 表がそのまま出る
build/fa_render --describe my_sound.yaml            # 組み上がった形・全 param の名前
```
JS では `fx.params`、Python では `fluent_audio.nodes()` / `Sound.params` に新しいノードの param がそのまま並びます。
