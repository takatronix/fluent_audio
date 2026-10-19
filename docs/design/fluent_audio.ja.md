# fluent_audio 設計書 — 声と UI 音のための宣言的リアルタイム音響効果

ステータス: **v0.1 草案（2026-10-19）— オーナー確認待ち**。§15 の未決事項を潰してから M1 に着手する。

**目的**: Kepler の声（と UI の効果音）に、見た目（look）ごとの「心地よい SF の音」を
**遅れなし**でかける。fluent_scene が「同じ絵をどこでも」なら、fluent_audio は
**「同じ音をどこでも」** — サーバ（native）でもブラウザ（wasm / AudioWorklet）でも、
同じ YAML から同じサンプル列が出る。

背景（オーナー 2026-10-19）:
「fluent audio みたいなかんじでオーディオのフィルタをあとでたそうか」
「遅くならないけど、心地よい SF サウンド」「ちょっとその辺真面目に設計しようかぁ」。

関連:
[fluent_scene 設計書](../../../fluent_scene/docs/design/fluent_scene.ja.md)（Scene / Stage / `$params` / `animate` / golden の規範。本書はこれに倣う）、
`fluent_vision_ros2/src/audio/`（fv_audio* — ROS 2 の入出力・VAD。本ライブラリはここに後で包まれる側）。

> **名前の注意**: fluent_vision_ros2 の中には ROS 2 の音声パッケージ群（fv_audio, fv_audio_output…、
> サービス型の名前空間 `fluent_audio/srv/...`）がある。本ライブラリは **ROS 非依存の新しい C++ ライブラリ**
> で、エフェクト・ミキシング・解析を担う。ROS 側は M4 でこれを包む（§14）。

---

## 0. 決定の要約（先に結論）

| # | 決定 | 理由（短く） |
|---|---|---|
| D1 | **C++17 の単一コア**、依存ゼロ（YAML パーサは fluent_scene の自作パーサを流用） | native / wasm / Python で「同じ音」を保証できるのは同じコードだけ |
| D2 | ブラウザでは **wasm を AudioWorklet の中で直接動かす**（SharedArrayBuffer 不要の方式） | ネットワーク 0・端末ごと・look の live param に追従・Safari / Tesla でも動く |
| D3 | **既定のアルゴリズム遅延 0 サンプル**。look-ahead は既定オフ | 「遅くならない」。声の dry 経路に遅延を持つノードを置かない |
| D4 | ブロック 128、**制御レート 32 サンプル（絶対サンプル番号で刻む）** | ホストの呼び出し単位が変わっても出力がビット一致（チャンク不変） |
| D5 | 宣言は **Sound YAML**（`schema: fluent.sound/v1alpha1`）: chain + sends(buses) + master、`$params` と `animate` | fluent_scene と同じ書き味。AI が安全に書け、実行中に param だけ動かせる |
| D6 | **look ごとのプリセットは全部ロード時に構築**し、切替は 300 ms のクロスフェード | 実行中のメモリ確保ゼロ。look 切替でブツッといわない |
| D7 | DSP は **全部自前実装**（MIT）。参照するのは MIT / BSD / PD のアルゴリズムと論文だけ | 小さい・決定的・ライセンスが綺麗。必要量はノード 15 個程度で自作の範囲 |
| D8 | 数学関数（sin / tanh / exp / log）も**自前の近似**、`-ffp-contract=off`、fast-math 禁止 | native と wasm でビット一致させるため（libm は環境で違う） |
| D9 | 解析（level / RMS / onset / 重心 / tail）を**グラフの出口で計算**して顔に渡す | 今の AnalyserNode の level 計測を置き換え、効果後の「実際に鳴っている音」で顔が動く |
| D10 | テストは fluent_scene と同じ **golden 契約**: オフラインレンダ → golden WAV、native↔wasm 一致 | 「同じ音」を機械で守る |

---

## 1. ゴール / 非ゴール

### ゴール
- **声のための効果**: TTS の声を look ごとに色付けする。聞き疲れしない、上品な SF。
  ギミック（ロボ声・ダーレク声・極端なピッチ）は既定にしない。
- **ミキシング**: 声 + UI 効果音 + （将来）BGM を1つのグラフで混ぜ、声の下で他をダッキング。
- **解析**: 鳴っている音の level / onset / 明るさ（スペクトル重心）を顔や UI に渡す。
- **同じ音をどこでも**: native（Linux / macOS、farm3 のサーバ、ファイル処理）と
  ブラウザ（wasm）で同じ出力。golden で検証。
- **宣言的**: Sound YAML で書き、`$params` を実行中に動かす（fluent_scene と同じ流儀）。
- **リアルタイム安全**: process() 内でメモリ確保・ロック・システムコールなし。

### 非ゴール（v1）
- DAW ではない（トラック・タイムライン・オートメーション編集 UI は作らない）。
- シンセサイザーではない（オシレータは ring-mod / LFO 用の内部部品だけ。UI 音の合成は v2 以降）。
- 音声認識・TTS・ノイズ除去 ML は持たない（STT は Whisper、TTS は AivisSpeech / Qwen3 のまま）。
- 線形位相 EQ・長い look-ahead・FFT 畳み込みリバーブは v1 では持たない（遅延とコストの理由）。
- デバイス入出力そのものは主目的ではない（native は miniaudio を薄く包むだけ。ROS の入出力は fv_audio の仕事）。

---

## 2. 用語 — fluent_scene との対応

| fluent_audio | 一言でいうと | fluent_scene の対応 |
|---|---|---|
| **Sound**（.fas / YAML） | 音の**宣言文書**。型検査・digest 付き | Scene |
| **Graph** | 実行中の**処理グラフ**（C++、保持型） | Stage |
| **Node** | グラフの1つの処理（eq, reverb…） | Layer（+ filter） |
| **Bus** | sends が流れ込む並列の枝（reverb などの wet 専用） | sublayers の合成先 |
| **Block** | 128 フレームの処理単位（float32、非インタリーブ） | Surface（出力の実体） |
| **Analysis** | ブロックごとの解析値（level 等） | （events / 読み戻し） |

```
 （AI・運用はここを編集）                  （C++ を書く人はここに直接）
┌─────────────────────┐  compile   ┌──────────────────────┐  process  ┌─────────────┐
│ Sound (.fas, YAML)  │ ─────────▶ │ Graph（ノードの鎖+bus）│ ────────▶ │ Block(128)  │
│ 型検査/digest/予算   │  検証→構築  │ 保持型・param だけ可変 │  毎ブロック │ + Analysis  │
└─────────────────────┘            └──────────────────────┘           └─────────────┘
                                        ▲             ▲
                    ROS / Python ───────┘             └─── fa_set_param_f32 / setParam
```

Graph の構造（ノードの並び・バッファ）は **compile 時にだけ** 作る。実行中に変えられるのは
param と、事前に構築済みのプリセット間の切替（D6）だけ。

---

## 3. どこで動くか

### 3.1 構成

```
                      ┌──────────── fluent_audio core (C++17, 依存ゼロ) ────────────┐
                      │ yaml → Sound → compile → Graph → process(block) → Analysis │
                      └───────┬──────────────────┬───────────────────┬────────────┘
                              │                  │                   │
             ┌────────────────┴───┐   ┌──────────┴─────────┐  ┌──────┴───────────┐
             │ wasm (Emscripten)  │   │ native             │  │ Python (nanobind)│
             │ AudioWorklet 内で  │   │ fa-render (CLI)    │  │ ツール・解析・   │
             │ 直接 instantiate   │   │ libfluent_audio.so │  │ プリセット調整   │
             │ Kepler のページ    │   │ miniaudio で I/O   │  │ テスト           │
             └────────────────────┘   └────────────────────┘  └──────────────────┘
               Mac / iPhone / Tesla     farm3 (server.py, G2,    （M4 で ROS 2 ノードも
                                        ファイル書き出し)          この C API を包む）
```

### 3.2 ブラウザ（主経路）— AudioWorklet + wasm

**推奨**: `AudioWorkletProcessor` の中で wasm を直接 instantiate する。

- wasm は **Emscripten の「Wasm Audio Worklets」API は使わない**。あれは SharedArrayBuffer
  （= COOP/COEP の cross-origin isolation）が必須で、Kepler のページは Cloudflare 越しの
  埋め込み iframe・外部画像を多用しており isolation を掛けると壊れる。
- 代わりに: メインスレッドで `fluent_audio.wasm` を fetch → `ArrayBuffer` を
  `AudioWorkletNode` の `processorOptions` で渡す → worklet の constructor で
  `new WebAssembly.Module(bytes)` + `new WebAssembly.Instance(...)`（同期。小さいので数 ms）。
  ビルドは `-sSTANDALONE_WASM`、libc 最小、`TextDecoder` / `fetch` に依存しない
  （AudioWorkletGlobalScope には無いブラウザがある）。文字列は UTF-8 バイト列で受け渡す。
- param とプリセット切替は `port.postMessage` → worklet 側で受けて **事前解決した
  param 番号**で `fa_set_param_idx()` を呼ぶ（名前の文字列検索を音声スレッドで毎回しない）。
- 解析値は worklet から `port.postMessage` で **4 ブロックごと（48 kHz で 10.7 ms、約 94 Hz）**に
  小さな Float32Array を送る（transfer しない固定長コピー。GC を出さないよう配列は使い回す）。
  cross-origin isolated なページなら SAB のリングに切り替えられるよう API は抽象化しておく。
- **注意（重要）**: worklet の `onmessage` は**音声スレッドで**走る。ここで YAML を
  パース・グラフ構築するとグリッチの危険がある → D6 のとおり **全 look のプリセットを
  起動時（最初の resume 前）に一括構築**し、実行中は「番号で切替」と「param 書き込み」だけにする。
  新しいプリセットの追加（AI が書いた Sound の差し替え）は、無音時またはクロスフェード中に
  1 つだけ構築する低頻度操作とし、構築時間を計測して予算外なら次の無音まで遅らせる。

### 3.3 native（サーバ・ファイル）

- `libfluent_audio`（静的 / 共有）+ `fa-render`（CLI: `fa-render preset.yaml in.wav out.wav --param mix=0.5`）。
- デバイス I/O が要る時だけ **miniaudio**（public domain / MIT-0）を使う薄い `fa-play`。
- farm3 の server.py は今 ffmpeg の `-af` を後段フィルタに使っている（voice.json の `filter`）。
  これを `fa-render` か Python バインディングに置き換えられる（§13.3）。
- 用途: G2 などブラウザを持たない出口、録画・動画への書き出し、golden の生成。

### 3.4 Python バインディング

- **nanobind**（BSD-3）。numpy 配列を受けて返す: `fa.Sound.load(path).render(x, sr, params)`。
- 用途: プリセット調整ツール、テスト、解析、server.py からの呼び出し、ROS ノードの試作。

### 3.5 リアルタイムの規則

| 項目 | 決定 |
|---|---|
| ブロック | **128 フレーム**（Web Audio の render quantum と同じ）。native は任意長を受け、内部で切る |
| 制御レート | **32 サンプル**ごとに param の補間値・フィルタ係数を更新。刻みは**絶対サンプル番号**基準 → ホストのチャンク分割に関わらず出力がビット一致 |
| process() の禁止事項 | メモリ確保、ロック、システムコール、例外、ログ出力、文字列処理。テストビルドで `operator new` をフックして**0回**を検証 |
| param の受け渡し | 単一生産者・単一消費者のロックフリーリング（固定長 256 件）。溢れたら古い同 param を上書き |
| denormal | wasm は FTZ を設定できない → **全フィードバック経路（IIR 状態・遅延線・包絡）で明示的に flush**（|x| < 1e-15 → 0）。native では FTZ/DAZ も立てるが、ビット一致のため頼らない |
| サンプルレート | **48 kHz と 44.1 kHz** を正式サポート（iPhone / Mac は 48k か 44.1k、TTS は 24k / 44.1k を decodeAudioData が変換）。係数は全部 Hz と ms で定義し SR から計算。32k〜96k は動くが golden なし |
| チャンネル | 入力 mono（声）/ stereo（UI 音）。グラフ内で mono→stereo に広げるノード（chorus, widen, reverb）を境に stereo |
| 数値 | float32。数学関数は自前の近似（D8）。`-ffp-contract=off`、`-ffast-math` 禁止 |
| SIMD | v1 はスカラー。wasm SIMD128 / NEON は v2 で、**結果が変わらない範囲**（並列化の順序を固定）でだけ |

### 3.6 遅延の予算（明記）

| 区間 | 追加遅延 |
|---|---|
| dry 経路（EQ, comp, de-esser, exciter, sat, gate, ring-mod, bitcrush, widen の M/S） | **0 サンプル** |
| AudioWorklet 自体 | **0**（render quantum の中で同期処理。グラフに遅延ブロックは足さない） |
| limiter | 既定 **0**（look-ahead なしのソフトクリップ併用）。`lookahead_ms: 1.5` を書いた時だけ 72 サンプル |
| chorus / Haas | dry は遅れない（wet 側だけが 5–20 ms ずれる。これは効果そのもの） |
| reverb / shimmer | wet のみ（predelay は意図した値。pitch shifter の窓遅延 ~30 ms は wet の中に隠れる） |

**結論: 声の頭は 1 サンプルも遅れない。** Graph は `latency_samples()` を返し、0 でないプリセットは
validate で warning を出す（`allow_latency: true` で黙らせる）。

CPU 予算: 1 プリセットあたり **128 フレーム処理が 48 kHz の 1 quantum（2.67 ms）の 5% 以内
（≈ 130 µs）を iPhone 12 / Tesla の Chromium で**。§4 の積算で最も重い fluent でも
~350 flop/サンプル ≈ 17 MFLOP/s で、余裕は 10 倍以上ある。

---

## 4. ノードカタログ v1（「心地よい SF の声」のため）

方針: どれも**小さく・決定的・ゼロ遅延**。コストは 48 kHz mono 1 サンプルあたりの概算演算数。

| ノード | アルゴリズム | 主な params（既定） | コスト | 遅延 |
|---|---|---|---|---|
| `eq` | RBJ biquad（TDF-II transposed）を最大 8 段。種類 hp / lp / bp / peak / lowshelf / highshelf / notch。係数は制御レートで補間 | `bands: [{type, f, q: 0.707, gain_db: 0}]` | 5×段数 | 0 |
| `compressor` | フィードフォワード、ソフトニー、log 領域、peak/RMS 混合の包絡（attack/release 別） | `threshold_db: -24, ratio: 2, attack_ms: 10, release_ms: 150, knee_db: 6, makeup_db: 0, key: self\|<bus>` | ~15（log/exp は制御レートで計算し補間） | 0 |
| `limiter` | 既定: 速い compressor + 最後に滑らかなソフトクリップ（tanh 近似、-1 dBFS 天井）。opt: look-ahead 1.5 ms + 4 倍オーバーサンプルの true-peak 推定 | `ceiling_db: -1, release_ms: 80, lookahead_ms: 0` | ~12（look-ahead 時 ~40） | 0（opt 72） |
| `deesser` | 分割帯域: HP（既定 6.5 kHz）だけを自分の帯域で圧縮して戻す（`x - hi + comp(hi)`）。位相が合うよう相補分割 | `freq: 6500, threshold_db: -30, ratio: 3` | ~25 | 0 |
| `exciter` | 高域（`freq` 以上）だけを tanh で倍音化し、もう一度 HP して少量足す（濁らない「空気」） | `freq: 4500, drive: 2, mix: 0.08` | ~20 | 0 |
| `saturate` | tanh / 非対称 soft-clip、レベル補償。v1 はオーバーサンプルなし（drive ≤ 3 に制限してエイリアスを抑える）。v2 で 2× ADAA | `drive: 1.5, mix: 1, asym: 0` | ~8 | 0 |
| `chorus` | 1–4 声の変調ディレイ（3 次 Hermite 補間）、LFO は sin 近似、声ごとに位相と速度をずらし L/R に配置。dry はそのまま | `voices: 2, rate_hz: 0.25, depth_ms: 2.5, base_ms: 12, mix: 0.3, spread: 1` | ~12×声数 | 0（wet のみ遅れ） |
| `doubler` | chorus の特殊形: 1 声、ランダム（平滑化ノイズ）で 8–25 ms と微小ピッチ揺れ | `delay_ms: 15, wobble: 0.3, mix: 0.25` | ~15 | 0 |
| `widen` | M/S 幅（mono 互換: モノに畳むと side が消えるだけ）+ 任意の Haas は **side にだけ**入れる（モノ和が変わらない） | `width: 1.0, haas_ms: 0, bass_mono_hz: 150` | ~6 | 0 |
| `reverb` | **8 線 FDN**（Householder 帰還、互いに素な長さ、線ごとに 1 次 LPF 減衰、遅延長をゆっくり変調してメタリック感を消す、入力に 4 段 allpass 拡散）。RT60 から線ごとの帰還ゲインを計算 | `decay_s: 1.2, size: 1, damp_hz: 6000, predelay_ms: 10, diffusion: 0.7, mod: 0.2, width: 1` | ~70 | 0（wet は predelay） |
| `shimmer` | 「`pitch_shift` → `reverb`」を束ねた糖衣。pitch shifter は 2 タップ遅延線＋sin² クロスフェード（Doppler 型、軽い）。帰還の中に入れる版（音が昇っていく）は `feedback` > 0 | `semitones: 12, mix: 0.2, decay_s: 2.8, feedback: 0, hp_hz: 600` | ~100 | 0（wet のみ ~30 ms） |
| `ringmod` | 低い周波数（20–80 Hz）の sin を**少量**かける（`1 - mix + mix·sin`）。電子っぽい粒。高い周波数はロボ声になるので range で 200 Hz までに制限 | `freq: 50, mix: 0.05` | ~4 | 0 |
| `formant` | **subtle** なフォルマント移動: v1 は 3 本の peak を動かす「疑似フォルマント EQ」（声道長を ±5% 程度に感じさせる）。本物の LPC / PSOLA は v2 | `shift: 0.0`（-1..1） | ~20 | 0 |
| `radio` | 帯域制限（HP 2 次 + LP 2 次）+ 中域の山 + 軽い saturate のプリセット化ノード。電話ほど狭くしない | `low_hz: 220, high_hz: 6000, presence_db: 3, drive: 1.8` | ~25 | 0 |
| `bitcrush` | ビット深度と sample-and-hold の降サンプル、**ごく少量**を wet で足す。ディザ（TPDF）付きで耳障りなトーンを避ける | `bits: 12, downsample: 1, mix: 0.05` | ~6 | 0 |
| `gate` | ヒステリシス付き（open/close 閾値）、hold、ゆっくり閉じる（range で完全に 0 にしない） | `open_db: -50, close_db: -56, hold_ms: 60, release_ms: 120, range_db: -24` | ~8 | 0 |
| `duck` | サイドチェーン圧縮: `key` の bus（例: voice）の包絡でこの経路を下げる | `key: voice, depth_db: -9, attack_ms: 5, release_ms: 250` | ~10 | 0 |
| `gain` / `pan` / `mix` | 基本部品（`$params` の受け口として使う） | `db: 0` / `pos: 0` / `amount: 1` | ~1 | 0 |
| `meter` | 解析ノード（§6）。音は通過のまま | `level, onset, centroid, tail` | ~10（重心は 256 点 FFT を 2 ブロックに 1 回） | 0 |

実装メモ:
- 共通の部品: 遅延線（2 冪のリングバッファ、補間 linear / Hermite）、1 次・2 次フィルタ、
  包絡、LFO（位相累積 + 多項式 sin）、乱数（xorshift32、seed 固定 → 決定的）、param スムーザ。
- 全ノードは `prepare(sr, max_block)` で必要メモリを**一度だけ**確保。遅延線の最大長は
  param の range から計算する（`size` や `base_ms` を上げても実行中に確保しない）。
- 設計の参考（コードは借りない）: RBJ Audio EQ Cookbook、Jot & Chaigne の FDN、
  Signalsmith「Let's Write a Reverb」（MIT の解説とコード）、Dattorro の plate、
  Zölzer『DAFX』の chorus / pitch shift。

---

## 5. Sound YAML

### 5.1 形

```yaml
schema: fluent.sound/v1alpha1
name: fluent
extends: voice_base          # 共通の前段（下）を継承。chain の先頭に展開される
io: { in: mono, out: stereo }

params:                      # fluent_scene と同じ書き方。setParam は animate の曲線で効く
  amount:  { type: f32, default: 1.0, range: [0, 1], runtime_mutable: true,
             animate: { duration: 0.4, ease: ease_in_out } }
  shimmer: { type: f32, default: 0.22, range: [0, 0.5],
             animate: { duration: 0.8, ease: ease_in_out } }
  speak:   { type: f32, default: 0, range: [0, 1], animate: { duration: 0.3 } }   # 顔と同じ気分の重み

chain:                       # 上から順に直列。send は「この地点から bus へ分岐」
  - eq: { bands: [ { type: lowshelf,  f: 180,  gain_db: -1.5 },
                   { type: peak,      f: 2800, q: 0.9, gain_db: 1.5 },
                   { type: highshelf, f: 9000, gain_db: 3 } ] }
  - exciter: { freq: 5000, drive: 2.5, mix: 0.10 }
  - send: { to: room,    gain: 0.10 }
  - send: { to: shimmer, gain: $params.shimmer }
  - chorus: { voices: 3, rate_hz: 0.18, depth_ms: 1.8, base_ms: 9, mix: 0.28 }     # mono → stereo
  - widen: { width: 1.3, bass_mono_hz: 150 }

buses:                       # 並列の wet 専用の枝。出力は master の前で chain と足される
  room:    [ reverb: { decay_s: 1.6, size: 0.8, damp_hz: 8000, predelay_ms: 18 } ]
  shimmer: [ shimmer: { semitones: 12, decay_s: 2.8, hp_hz: 600, mix: 1 } ]

master:
  - mix: { amount: $params.amount }        # 0 = dry、1 = このプリセットそのもの（全経路の wet/dry）
  - limiter: { ceiling_db: -1 }

analysis: { level: true, onset: true, centroid: true, tail: true, rate_hz: 94 }
```

規則:
- **chain / buses / master の3段だけ**。任意の DAG は v1 では書けない（読みやすさ・検証・
  コスト見積もりのため）。bus から bus への send は 1 段まで（ループ禁止を型で保証）。
- `mix.amount` は master で「この Sound 全体の効き」を決める特別扱い: Graph は入力を
  dry としてコピーしておき、`out = dry·(1-a) + wet·a` を等パワーで混ぜる（a=0 で完全に素通し）。
- param の型: `f32`, `bool`, `enum`（例: `type: enum, values: [kepler, fluent, ...]`）。
  範囲外は validate で拒否、`range` は必須（遅延線の確保量の根拠になる）。
- `animate`: `duration`（秒）と `ease`（linear / ease_in / ease_out / ease_in_out）。
  中身は fluent_scene と同じ「目標値 + 現在値 + 曲線」。進行中の再設定は現在値から繋ぐ。
  音では補間を**制御レート（32 サンプル）で評価し、ゲイン系はその間をサンプル単位で線形補間**
  （ジッパーノイズを出さない）。`animate` が無い param も最低 20 ms のスムージングを掛ける
  （音では即時変更 = クリックなので、「即時」は存在しない）。
- digest: fluent_scene と同じく並べ替え不変の指紋（params の順や空白に依存しない）。
- 予算: validate が各ノードのコスト（§4 の表）を積算して `budget: { flops_per_sample: 600 }` を超えたら拒否。

### 5.2 プリセット（look ごと）の束

```yaml
schema: fluent.sound/v1alpha1
bank: kepler_looks
presets: [ kepler, luna, fluent, kitt, hal, gargantua, aurora, system, comic ]   # ファイル名 = presets/<name>.yaml
crossfade: { duration: 0.3, ease: ease_in_out }   # 切替は等パワーのクロスフェード（両方を 300 ms だけ並走）
```

Graph はバンクの全プリセットを compile 時に構築し（メモリは合計で数 MB 以下: 一番大きい reverb の
遅延線でも 48 kHz で 8 線 × 最大 6 万サンプル × 4 B ≈ 1.9 MB）、`fa_select(inst, "fluent")` で切り替える。
切替中は 2 プリセット分の CPU を使うので、予算は「最大 2 つ同時」で見積もる。
リバーブの尾は旧プリセット側で自然に減衰させる（ブツ切りにしない）。

### 5.3 C API（fluent_scene の `fs_*` と同じ形）

```c
FaInstance* fa_create(const char* yaml_utf8, float sample_rate, int max_block);  // 失敗で NULL
const char* fa_error(void);                         // 直前の fa_create の失敗理由
void        fa_destroy(FaInstance*);
const char* fa_digest(FaInstance*);
int         fa_latency_samples(FaInstance*);

// 入出力（wasm では JS が HEAPF32 に書いて読む。非インタリーブ）
float*      fa_in_buffer(FaInstance*, int channel);   // max_block 長
float*      fa_out_buffer(FaInstance*, int channel);
int         fa_process(FaInstance*, int frames);      // RT 安全。1 = ok

// param（fs_set_param_* と同じ意味: animate の曲線で効く）
int fa_set_param_f32(FaInstance*, const char* name, float v);   // 非 RT スレッド用（名前解決あり）
int fa_set_param_bool(FaInstance*, const char* name, int v);
int fa_param_index(FaInstance*, const char* name);              // 事前に番号を引く
int fa_set_param_idx(FaInstance*, int index, float v);          // RT 安全版
int fa_select(FaInstance*, const char* preset);                 // バンク内の切替（クロスフェード）

// 解析（§6）と診断
const float* fa_analysis(FaInstance*);             // 最新ブロックの FaAnalysis（固定レイアウト）
const char*  fa_diagnostics(FaInstance*);          // 直前の setter が 0 を返した理由（読むと空になる）
const char*  fa_drain_events(FaInstance*);         // onset / tail_end などのイベント（非 RT で読む）
```

C++ 側（`namespace fluent_audio`）:

```cpp
auto parsed   = fluent_audio::sound::parse(yaml);       // 誤りは全部ここで拒否
auto compiled = fluent_audio::sound::compile(parsed.doc, {.sample_rate = 48000});
Graph& g = *compiled.graph;
g.setParam("shimmer", 0.15f);                            // animate で効く
g.process(in, out, frames);                              // RT 安全
const Analysis& a = g.analysis();
```

---

## 6. 解析出力 — 顔を動かす値

今は tts.js が `AnalyserNode` の RMS × 4 を level として顔に渡している（約 60 Hz、効果前の音）。
これを **グラフの出口（master の後）で計算した値**に置き換える。顔は「実際に鳴っている音」に合う。

| 値 | 定義 | 用途 |
|---|---|---|
| `level` | RMS（10 ms 窓）を dBFS → 0–1 に写像（-60 dB=0、-6 dB=1）、attack 5 ms / release 120 ms | 口・発光の大きさ（今の level の置き換え） |
| `peak` | ブロック内の最大絶対値 | クリップ監視 |
| `onset` | 包絡の対数微分がしきい値を越えた瞬間（音節の頭）。不応期 60 ms | 口のパク、粒の発生 |
| `centroid` | スペクトル重心 Hz（256 点 FFT、2 ブロックごと）→ 0–1（300 Hz–5 kHz の対数） | 明るさ → 顔の色温度・きらめき |
| `voiced` | level > しきい値 かつ 重心が声の範囲 | 「話している」判定 |
| `tail` | 出力が -60 dBFS 未満に落ちるまでの残響中フラグ | 「自分の声が鳴っている」判定をリバーブの尾まで延ばす（§13.4 マイクの自己聴取対策） |

- 固定レイアウトの `struct FaAnalysis { float level, peak, onset, centroid, voiced, tail; uint32 frame; }`。
- 送る頻度 ~94 Hz（4 ブロック）。顔のフレームに合わせる時は `AudioContext.outputLatency`
  （取れない Safari では `baseLatency` + 10 ms の推定）だけ**解析を遅らせて**描く → 口と音が合う。
- fluent_scene の `$params.level` / `speak` に直結できる（Liquid の level と同じ意味）。

---

## 7. look ごとのプリセット（初期値）

全プリセット共通の前段 `voice_base`: `eq hp 80 Hz` → `compressor（-24 dB, 2:1, 10/150 ms, +2 dB）`
→ `deesser（6.5 kHz, -32 dB, 3:1）`。最後は `limiter -1 dBFS`。**音量は全 look で揃える**
（プリセットごとに `makeup` を測って固定し、golden の loudness テストで ±1 LU を守る）。

| look | 狙い | 中身（chain / buses） | 試作 |
|---|---|---|---|
| **kepler** | いつもの声を「少しだけ良く」。きれい＋空気 | peak 3.2 kHz +1.5 / highshelf 10 kHz +2.5 / exciter 4.5 kHz 0.08 / 小部屋 reverb 0.5 s を -31 dB | ✓ |
| **luna** | ルナ自身の声（Qwen3）。**素のまま** | voice_base だけ（+ limiter）。効果なし | – |
| **fluent** | 柔らかい液体。きらめき | 上の §5.1 の例そのもの（コーラス 3 声・幅 1.3・室 1.6 s・1 オクターブ上のシマー -22 dB） | ✓ |
| **kitt** | 車載コンピュータ。無線の帯域＋軽い歪み | radio（220 Hz–6 kHz、1.4 kHz +3）/ saturate 1.8 mix 0.6 / ringmod 55 Hz 0.05 / 車内の短い反射 0.35 s | ✓ |
| **hal** | 落ち着いた、暖かい、近い。冷たくしない | lowshelf 200 Hz +1.5 / peak 3 kHz -1（耳当たりを丸く）/ highshelf 9 kHz -1 / doubler なし / 小部屋 0.7 s を -26 dB / compressor をもう少し深く（3:1）で「近接効果」 | – |
| **gargantua** | 深宇宙。遠くまで伸びる暗い残響を**薄く** | lowshelf 160 Hz +2 / 暗い大空間 reverb 5.5 s（damp 3.5 kHz、predelay 60 ms）を -17 dB / 1 オクターブ下のシマー 6 s を薄く | ✓ |
| **aurora** | 夜空の光のカーテン。高く透明 | highshelf 8 kHz +3 / chorus 2 声 0.12 Hz / シマー +12 と +19（5 度上）を交互に薄く / reverb 2.2 s 明るめ（damp 10 kHz） | – |
| **system** / **comic** | 未定。当面 kepler と同じ | `extends: kepler` | – |

試作（✓）は numpy の sketch で作った（`sketch/render.py`、§12）。試聴: `media/faudio.view.json`。
各 YAML は `presets/*.yaml` に置いた（v1alpha1 の書き方の見本を兼ねる）。

look 以外の param（全プリセットが持つ）:
`amount`（効きの強さ 0–1、設定画面のスライダーにする）、`speak` / `think`（顔と同じ気分の重み —
例: fluent は think 中に shimmer を少し上げる、程度の**ごく小さな**連動に留める）。

---

## 8. 「心地よい」を守る規則（品質の線引き）

fluent_scene のデザインリンターに相当する **サウンドリンター** を validate に入れる:

1. **明瞭度**: 声の帯域（300 Hz–4 kHz）の dry 成分が、どの param の組でも wet より 10 dB 以上大きい
   （STI の簡易代理。リバーブで言葉を濁さない）。
2. **耳当たり**: 2–5 kHz を合計で +4 dB 以上持ち上げたら warning（刺さる帯域）。
3. **ピッチ**: dry 経路にピッチを変えるノードを置いたら warning（声のキャラが変わる。`allow_pitch: true`）。
4. **モノ互換**: L+R を足した時の loudness 低下が 1.5 LU 以内（Tesla の車内・iPhone のスピーカーは実質モノ）。
5. **音量**: プリセット間の integrated loudness 差 ±1 LU（look を変えて声が大きくなる/小さくなるのを防ぐ）。
6. **遅延**: `latency_samples > 0` は warning（D3）。
7. **ring-mod / bitcrush の mix 上限**: range で 0.15 まで（ギミック化の防止）。

---

## 9. テスト

| 種類 | 中身 | 合格基準 |
|---|---|---|
| **golden（オフライン）** | 固定入力（インパルス、対数スイープ、ピンクノイズのバースト、無音→音→無音、CC0 の朗読 10 s と Kepler の声）× 全ノード × 全プリセット × {44.1k, 48k} → `tests/golden/*.wav`（float32） | **null テスト**: 差分の RMS ≤ -100 dBFS、最大 ≤ -90 dBFS。golden の更新は fluent_scene と同じく理由付きのコミットだけ |
| **native ↔ wasm** | 同じ入力を native と wasm（node で実行）で処理 | **ビット一致**を目標（D8）。許容は max\|Δ\| ≤ 2⁻²⁴（-144 dB） |
| **チャンク不変** | 同じ入力を 128 固定 / 1〜512 のランダム長で process | ビット一致（D4） |
| **RT 安全** | `operator new` / `malloc` のフック、mutex のフック | process 中 0 回 |
| **denormal** | インパルスの後に無音 120 s | ブロックあたりの処理時間が最初と最後で同じ（±10%） |
| **param ファズ** | ランダムな時刻に全 param をランダム値で setParam | NaN / Inf なし、サンプル間の差分が 0.3 を超えない（クリックなし） |
| **品質（§8）** | loudness（ITU-R BS.1770 を自前実装）、モノ互換、明瞭度 | §8 の数値 |
| **ベンチ** | ノード別・プリセット別の ns / block。farm3（x86）、Mac、node の wasm、ブラウザのベンチページ（Chrome / Safari / Tesla / iPhone） | §3.6 の 5% 予算。CI で 20% 以上の悪化は fail |
| **主観（記録）** | 新プリセットは A/B（dry との）を試聴ページに追加し、オーナーの OK を記録 | — |

TTS の声のライセンス: AivisSpeech のモデルは利用規約がモデルごとに違う。golden に入れる
声は **自分たちで録る CC0 の朗読**か、規約で再配布が許されたモデルの出力に限る（Kepler の
声のクリップはローカルの主観テストにだけ使い、リポジトリに入れない）。

---

## 10. ライセンス — 何を使い、何を自分で書くか

本体は **MIT**（© takatronix、fluent_scene と同じ）。

| 候補 | ライセンス | 判断 |
|---|---|---|
| **miniaudio** | public domain / MIT-0 | **使う**（native のデバイス I/O と WAV/MP3/FLAC の読み込み。単一ヘッダ） |
| **dr_wav / dr_mp3** | public domain / MIT-0 | miniaudio に同梱のものを使う |
| **nanobind** | BSD-3 | **使う**（Python） |
| fluent_scene の YAML パーサ | MIT（自作） | **流用**（依存を増やさない。将来 fluent_core に切り出す候補） |
| Signalsmith DSP / Signalsmith Stretch | MIT | **参考にする / v2 で検討**。v1 の pitch shift は Doppler 型で足りる。高品質なピッチ（formant 保持）が要る時に Stretch を入れる候補 |
| Freeverb | public domain | **参考のみ**（金属的で古い。FDN の方が良い） |
| Airwindows | MIT | **参考のみ**（音は良いがコードは 1 プラグイン 1 ファイルの独特な形で、param 体系・決定性の流儀が違う。saturation / 空気感の考え方を学ぶ） |
| DaisySP | MIT（一部モジュールは DaisySP-LGPL に分離） | **参考のみ**。使うなら MIT 側だけで、LGPL 側は入れない |
| pocketfft / KissFFT / PFFFT | BSD-3 / BSD-3 / BSD 系 | v1 は 256 点の FFT 1 本だけなので**自作**（radix-2、数十行）。v2 で畳み込みが要れば PFFFT |
| r8brain-free-src / libsamplerate | MIT / BSD-2 | v1 は不要（ブラウザが変換、サーバは 48k 固定）。ファイルツールで要れば r8brain |
| **JUCE** | AGPLv3 / 商用 | **使わない** |
| **Faust** の生成コード | 元の .dsp と使う標準ライブラリのライセンスを継ぐ（GPL / LGPL / STK を含む） | **使わない**（生成コードに GPL が混ざる危険） |
| Rubber Band | GPL / 商用 | **使わない** |
| SoundTouch | LGPL-2.1 | **使わない**（wasm への静的リンクで LGPL の再リンク義務が面倒） |
| KFR | GPL / 商用 | **使わない** |
| RNNoise | BSD-3 | v1 では不要（ノイズ除去は TTS には要らない）。マイク経路を扱う時の候補 |

`THIRD_PARTY.md` を fluent_scene と同じ形で最初から置く。

---

## 11. ディレクトリ構成（予定）

```
fluent_audio/
  include/fluent_audio/   graph.hpp node.hpp params.hpp analysis.hpp sound/{document,compiler}.hpp fa.h
  src/                    core/（遅延線・フィルタ・包絡・数学近似）nodes/（§4 を 1 ノード 1 ファイル）sound/
  wasm/                   build.sh  worklet.js（AudioWorkletProcessor）fluent-audio.js（メイン側の薄いラッパ）demo.html
  python/                 nanobind モジュール
  tools/                  fa-render  fa-bench  fa-golden
  presets/                kepler.yaml fluent.yaml … looks.yaml（バンク）
  tests/                  golden/  test_*.cpp
  sketch/                 今回の numpy 試作（M1 で C++ に置き換えたら削除）
  docs/                   design/ getting-started.ja.md
```

---

## 12. 今回の試作（sketch）

`sketch/render.py`（numpy / scipy）で dry / kepler / fluent / kitt / gargantua の 5 つを
オフラインで作った。ノード名・param 名・既定値は本書の §4 / §7 に合わせてある。
FDN は本番と同じく 128 サンプルのブロックで回している。音量は dry に揃えた（integrated で ±0.9 LU）。

客観チェック（`/tmp` の簡易解析）: クリップなし、NaN なし、L/R の相関 0.976 以上（モノ互換良好）、
リバーブの尾は減衰して終わる。

**素材について**: 今の Kepler の声（AivisSpeech）は 8 kHz より上の成分がほとんど無い（全体の 0.1%）。
high shelf で「空気」を足しても持ち上げる物が少ないので、exciter（倍音を作る）が効く。
これは本番プリセットでも同じ判断になる。

---

## 13. Kepler への組み込み計画

### 13.1 今の経路（2026-10-19 時点）

- `public/tts.js`: `/api/tts` で文ごとに WAV を取り → `decodeAudioData` → `AudioBufferSourceNode` →
  `AnalyserNode`（fftSize 512、level = RMS×4）→ `destination`。文を先読みしてパイプライン再生。
- `agent.html` は speaking 中 `NeuralTTS.level()` を顔（faceWin）に毎フレーム渡す。
- サーバ（`server.py`）は voice.json の `filter`（今は `loudnorm=I=-20...,aresample=48000`）を ffmpeg で掛けてから返す。
- look は `agent-ext.js` の LOOKS（kepler / luna / aurora / fluent / kitt / hal / comic / system）と `mode.sh` / `mode.json`。

### 13.2 ブラウザ（M1–M2）

```
AudioBufferSourceNode(文) ─▶ AudioWorkletNode('fluent-audio') ─▶ destination
                                   │ port: {select: look} {param: amount} ↑ {analysis}
```

- `tts.js` の `out()` を「worklet があればそれ、無ければ今の analyser」に置き換える（**機能フラグ**
  `localStorage['kepler.faudio']`、既定オフ → オーナーが聞いて OK したら既定オン）。
- `level()` は worklet から届いた最新の `analysis.level` を返す（同じ 0–1 の意味。顔側は変更なし）。
  余裕があれば `onset` / `centroid` も faceWin のメッセージに足す（各顔が使うかは顔ごとに後で）。
- look が変わったら（mode.json を読んだ時 / CHARA の選択時）`port.postMessage({select: look})`。
- 1 つの Graph を**会話の間ずっと生かしておく**ので、文と文の間でリバーブの尾が自然につながる
  （今は文ごとに音が切れる）。UI 音も同じ worklet の別入力に入れれば声の下でダッキングできる（M2）。
- 失敗時（AudioWorklet なし・wasm 読み込み失敗）は今の経路に落ちる。**音が出ない状態は作らない**。
- サーバの `loudnorm` はそのまま残す（入力の音量を揃える役。fluent_audio は揃った声を前提にする）。

### 13.3 サーバ / G2 / ファイル（M3）

- G2 のゲートウェイは今は**声を返していない**（文字だけ）。声を返す時、または動画・ファイル書き出しの時は
  サーバで native の fluent_audio を掛ける: voice.json に `"fa_preset": "kitt"` を書けば
  `post_filter` が ffmpeg の代わりに（または後に）`fa-render` / Python バインディングを呼ぶ。
- **二重掛けの防止**: サーバで掛けた WAV にはレスポンスヘッダ `X-FA-Applied: <preset>@<digest>` を付け、
  ページはそれを見たら worklet を `amount 0`（素通し）にする。

### 13.4 気をつけること

- **自分の声を自分で聞く問題**: agent-ext.js はマイクの言葉を「自分の声が鳴っている間」は捨てている。
  リバーブの尾（gargantua で最大 ~1.5 s 聞こえる）の間もマイクに声が入るので、
  `speaking` の終わりを `analysis.tail` が落ちるまで延ばす（上限 1.5 s）。
- **iPhone Safari**: AudioContext は tap で resume（tts.js が既にやっている）。消音スイッチで Web Audio が
  黙る問題は `navigator.audioSession.type = "playback"`（Safari 17+）で回避できる — 今と同じ挙動に
  するかはオーナー判断（§15）。`audioWorklet.addModule` は secure context が要る（Kepler は https / localhost で OK）。
- **Tesla**: Chromium だが CPU が弱め。ベンチページで 5% 予算を実測してから既定オンにする。
- **バックグラウンドのタブ**: worklet は止まり得る。復帰時に Graph の状態を `reset()`（尾の残りを捨てる）。

---

## 14. マイルストーン

| M | 中身 | 完了の定義 |
|---|---|---|
| **M1** | C++ コア（Graph、param + animate、制御レート、denormal 対策、自前の数学近似）+ **5 ノード**（eq, compressor, deesser, reverb, limiter）+ Sound YAML（chain / buses / master）+ wasm worklet + Kepler の tts.js に機能フラグ付きで接続 + level 解析で顔を駆動 | golden（native）と native↔wasm 一致と RT 安全テストが緑。iPhone / Mac / Tesla で kepler プリセットが鳴り、顔が動く |
| **M2** | 残りのノード（exciter, saturate, chorus, doubler, widen, shimmer, ringmod, formant, radio, bitcrush, gate, duck, meter 全項目）+ **全 look のプリセットとバンク切替（クロスフェード）**+ サウンドリンター + UI 音のダッキング | 全プリセットの golden と §8 の品質テストが緑。オーナーが試聴ページで各 look を OK |
| **M3** | native の `fa-render` / `fa-play`（miniaudio）+ server.py の `fa_preset` + `X-FA-Applied` + G2 / ファイル書き出しの経路 | サーバで掛けた音とブラウザで掛けた音がビット一致（同じ SR で） |
| **M4** | Python バインディング（nanobind）+ プリセット調整ツール + ROS 2 ラッパ（fv_audio_output の前段に入るノード。ROS の param で `$params` を動かす） | `pip install` で使える。ROS ノードが fv_audio_output と繋がる |

M1 の見積もり: コア + 5 ノード + YAML + worklet で集中して数日。M2 はノードが小さいので 1 ノード数時間 × 13。

---

## 15. 未決事項（オーナーに聞きたいこと）

1. **既定の効き**: 各 look の効きは今回の試聴くらい（控えめ）でよいか。もっと「SF っぽさ」を前に出すか。
2. **どこで掛けるか**: 推奨は「ブラウザの worklet（端末ごと・live）を主、サーバは G2 / ファイル用」。
   サーバで一括（全端末同じ・実装が少し簡単・ただし look 切替に追従しにくい）を好むか。
3. **luna** は効果なし（素の声）で確定でよいか。comic / system の音は何にするか。
4. **UI 効果音**（タップ音・通知音）も fluent_audio で鳴らし、声の下でダッキングするか（M2 に入れるか）。
5. **iPhone の消音スイッチ**: 今は消音で Kepler の声も黙るはず。fluent_audio 導入時に「消音でも鳴る」にするか。
6. **名前**: リポジトリ / 名前空間は `fluent_audio`（C 接頭辞 `fa_`、YAML は `fluent.sound/v1alpha1`、拡張子 `.fas`）で
   よいか。fluent_vision_ros2 側の既存の `fluent_audio/srv/...` 名前空間とは M4 で揃える（ROS 側を改名するか）。
7. **公開**: fluent_scene と同じく GitHub で MIT 公開する前提でよいか（今はローカルの git だけ）。
8. **声と顔の同期**: 解析を出力遅延ぶん遅らせて口を合わせる（§6）。今の顔は遅延補正なしで見慣れているので、
   変えた時に違和感が無いか見てほしい。
