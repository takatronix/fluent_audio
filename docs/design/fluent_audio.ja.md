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
| D3 | **追加のアルゴリズム遅延は既定 0 サンプル**。look-ahead は既定オフ | 「遅くならない」。声の dry 経路にバッファ遅延を持つノードを置かない（端末・ブラウザ自身の出力遅延は別勘定で、実測して記録する） |
| D4 | ブロック 128、**制御レート 32 サンプル（絶対サンプル番号で刻む）**、param 変更は**サンプル時刻付きのイベント** | ホストの呼び出し単位が変わっても出力がビット一致（チャンク不変） |
| D5 | 宣言は **Sound YAML**（`schema: fluent.sound/v1alpha1`）: chain + sends(buses) + master、`$params` と `animate` | fluent_scene と同じ書き味。AI が安全に書け、実行中に param だけ動かせる |
| D6 | **look ごとのプリセットは全部ロード時に構築**し、切替は 300 ms のクロスフェード | 実行中のメモリ確保ゼロ。look 切替でブツッといわない |
| D7 | DSP は **全部自前実装**（MIT）。参照するのは MIT / BSD / PD のアルゴリズムと論文だけ | 小さい・決定的・ライセンスが綺麗。必要量はノード 15 個程度で自作の範囲 |
| D8 | 数学関数（sin / tanh / exp / log）も**自前の近似**、`-ffp-contract=off`、fast-math 禁止 | native と wasm でビット一致させるため（libm は環境で違う） |
| D9 | 解析をグラフの中で計算して顔に渡す: **口は声そのもの（効果前）の包絡**、光・きらめきは**出口の音**（level / 重心）、別に残響中フラグ `tail` | 今の AnalyserNode を置き換える。口がリバーブの尾でパクパクしない |
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
- 代わりに: **メインスレッドで** `WebAssembly.compileStreaming` して得た `WebAssembly.Module` を
  `AudioWorkletNode` の `processorOptions` で渡し、worklet の constructor では
  `new WebAssembly.Instance(module, imports)` だけをする（コンパイルを音声スレッドでしない）。
  Module を渡せないブラウザがあれば bytes を渡してその場でコンパイルする — どちらも
  **最初の再生（resume）より前**に済ませ、`port` で `{ready}` / `{error}` を返す握手を必ず入れる。
  ビルドは `-sSTANDALONE_WASM`、libc 最小、import は明示した数個だけ、`TextDecoder` / `fetch` に
  依存しない（AudioWorkletGlobalScope には無いブラウザがある）。文字列は UTF-8 バイト列で受け渡す。
  **wasm のメモリは固定長（16 MB、grow 禁止）**にし、HEAPF32 のビューは constructor で一度だけ作って使い回す。
- `process()` は渡された**実際のフレーム数**を読む（128 を決め打ちしない。Chrome は render quantum を
  変えられるようになりつつある）。入力が繋がっていない時は無音として処理し、チャンネル数は
  `numberOfInputs: 1, outputChannelCount: [2]` を明示。`process()` は常に `true` を返して
  文と文の間の無音・リバーブの尾の間もノードを生かす。
- param とプリセット切替は `port.postMessage` → worklet 側で受けて **事前解決した番号**で
  `fa_set_param_idx()` / `fa_select_idx()` を呼ぶ（名前の文字列検索を音声スレッドでしない）。
  連続値はメインスレッド側でまとめ（同じ param はフレームごとに最新 1 件）、送る頻度に上限を設ける。
- 解析値は worklet から `port.postMessage` で **画面の頻度（約 60 Hz = 48 kHz で 6 ブロックごと）**に
  小さな配列を送る。postMessage は構造化複製でどうしても割り当てが起きる（配列を使い回しても
  ゼロにはならない）ので、頻度を絞って GC 負荷を抑え、GC を起こしながらのグリッチ試験（§9）で確かめる。
  cross-origin isolated なページなら SAB のリング（割り当てゼロ）に切り替えられるよう API は抽象化しておく。
- **注意（重要）**: worklet の `onmessage` は**音声スレッドで**走る。ここで YAML を
  パース・グラフ構築すると、クロスフェード中でも CPU が足りずに途切れ得る → D6 のとおり
  **全 look のプリセットを起動時（最初の resume 前）に一括構築**し、実行中は「番号で切替」と
  「param 書き込み」だけにする。新しい Sound の差し替え（AI が書いたもの）は、**声も尾も鳴っていない
  時（`tail` が 0）にだけ**構築し、その間の出力は無音でよいことを前提にする（構築中は再生を始めない）。
- 失敗時: `processorerror`・初期化失敗・`AudioContext` の interrupted / suspended を受けたら、今の
  直結経路（source → analyser → destination）に繋ぎ直す。バックグラウンドから戻った時は状態を捨てず、
  出力を 20 ms でフェードインして再開する（いきなり reset するとクリックになる）。

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
| 制御レート | **32 サンプル**ごとに param の補間値・フィルタ係数を更新。刻みは**絶対サンプル番号**（64 bit のカウンタ）基準。包絡・FFT の hop・乱数の消費・プリセット切替・解析の窓もすべてサンプル時刻で刻む |
| param イベント | 各変更は**適用サンプル時刻**を持つ（`fa_set_param_at(inst, idx, v, frame)`。時刻なしは「次の制御刻み」）。同時刻は受け付け順。過去の時刻（遅着）は次の制御刻みで適用し diagnostics に数える。→ 同じ時刻付きイベント列なら、チャンク分割に関わらず出力がビット一致 |
| process() の禁止事項 | メモリ確保、ロック、システムコール、例外、ログ出力、文字列処理。テストビルドで `operator new` / `malloc` をフックして**0 回**を検証 |
| param の受け渡し | 連続値は **param ごとの「最新値 + 版番号」スロット**（atomic、上書きしても競合しない）。順序が意味を持つもの（select / reset / bypass、時刻付きイベント）は別の SPSC キュー（固定長 256、溢れたら拒否して diagnostics）。生産者は 1 スレッド（worklet では port、native では呼び出し側）と決める |
| denormal | wasm は FTZ を設定できない → **全フィードバック経路で明示的に flush**: IIR の状態・遅延線への書き込み・包絡・補間の途中値で `|x| < 2⁻¹⁰⁰（≈ 8e-31、-600 dB）→ 0`。これは意図した尾の打ち切りで、場所は全ターゲット共通（コードで1関数）。native の FTZ/DAZ は**立てない**（立てると flush 前の途中計算が wasm と変わる） |
| 非有限値 | 入力の NaN / Inf は 0 に置き換え、内部状態に NaN が出たらそのノードを reset して diagnostics に記録（声が永遠に黙る事故を防ぐ） |
| サンプルレート | **48 kHz と 44.1 kHz** を正式サポート（AudioContext の実際の `sampleRate` を使う）。係数は全部 Hz と ms で定義し SR から計算。それ以外は fa_create が拒否（v1）。TTS（24k / 44.1k）の変換はブラウザの decodeAudioData がするので、**ブラウザとサーバの出力は「変換後の同じ PCM を入れた時だけ」ビット一致**する（変換器が違う） |
| チャンネル | 入力 mono（声）/ stereo（UI 音）。グラフ内で mono→stereo に広げるノード（chorus, widen, reverb）を境に stereo |
| 数値 | float32 で計算、係数の生成だけ double。数学関数は自前の多項式近似（sqrt だけは IEEE で正確丸めなので標準を使う）。`-ffp-contract=off`（FMA 禁止）、`-ffast-math` 禁止、x87 禁止（x86-64 は SSE2）、演算順はコードの順に固定。対象は x86-64 / aarch64 / wasm32 の3つで、ここの間のビット一致を CI で見る |
| SIMD | v1 はスカラー。wasm SIMD128 / NEON は v2 で、**結果が変わらない範囲**（並列化の順序を固定）でだけ |

### 3.6 遅延の予算（明記）

| 区間 | 追加遅延 |
|---|---|
| dry 経路（EQ, comp, de-esser, gate, ring-mod, bitcrush, widen の M/S） | **0 サンプル** |
| saturate / exciter / radio（ADAA） | 0.5 サンプル（48k で 0.01 ms。latency_samples には切り捨てで 0 と報告し、golden の位相に含める） |
| AudioWorklet 自体 | **0**（render quantum の中で同期処理。グラフに遅延ブロックは足さない） |
| limiter | 既定 **0**（look-ahead なしのソフトクリップ併用）。`lookahead_ms: 1.5` を書いた時だけ 72 サンプル |
| chorus / Haas | dry は遅れない（wet 側だけが 5–20 ms ずれる。これは効果そのもの） |
| reverb / shimmer | wet のみ（predelay は意図した値。pitch shifter の窓遅延 ~30 ms は wet の中に隠れる） |

**結論: fluent_audio が足す遅延は 0 サンプル（声の頭は遅れない）。** ただし IIR フィルタには
周波数ごとの位相遅れ（群遅延）があり、hp 80 Hz で低域に ~2 ms、声の帯域では 0.1 ms 未満 — 聞いて分かる
量ではないが「0」とは別に記録する。端末・ブラウザ自身の出力遅延（`AudioContext.outputLatency` 等）は
fluent_audio の外の話で、ベンチページで端末ごとに実測して載せる。
Graph は `latency_samples()` を返し、0 でないプリセットは validate で warning を出す（`allow_latency: true`）。
look-ahead のサンプル数は実際の SR から計算する（1.5 ms = 48k で 72、44.1k で 66）。

CPU 予算: **バンク切替中（2 プリセット同時）でも、1 quantum（48 kHz で 2.67 ms）の処理が 5% 以内
（≈ 130 µs）を iPhone 12 / Tesla の Chromium で**。§4 の演算数の積算（最も重い fluent で
~350 演算/サンプル）は目安にすぎない（数学近似・メモリアクセス・FFT のまとめ処理・JS のコピーと
メッセージを含まない）ので、**判定は実機の最悪値**（切替中・尾が残る中・GC を起こしながら）で行う。

メモリ: reverb の長い残響は帰還ゲインで作るので遅延線は短い（1 本最大 ~6,000 サンプル、size 1.8 で）。
1 reverb ≈ 8 線 × 8 K（2 冪に丸め）× 4 B ≈ 256 KB、chorus / shimmer は数十 KB。looks バンク全体で
**2 MB 程度**の見込み。wasm のメモリ 16 MB 固定に十分収まる（実測して確定する）。

---

## 4. ノードカタログ v1（「心地よい SF の声」のため）

方針: どれも**小さく・決定的・ゼロ遅延**。コストは 48 kHz mono 1 サンプルあたりの概算演算数。

| ノード | アルゴリズム | 主な params（既定） | コスト | 遅延 |
|---|---|---|---|---|
| `eq` | RBJ biquad（TDF-II transposed）を最大 8 段。種類 hp / lp / bp / peak / lowshelf / highshelf / notch。係数は制御レートで補間 | `bands: [{type, f, q: 0.707, gain_db: 0}]` | 5×段数 | 0 |
| `compressor` | フィードフォワード、ソフトニー、log 領域、peak/RMS 混合の包絡（attack/release 別） | `threshold_db: -24, ratio: 2, attack_ms: 10, release_ms: 150, knee_db: 6, makeup_db: 0, key: self\|<bus>` | ~15（log/exp は制御レートで計算し補間） | 0 |
| `limiter` | **安全リミッタ**（透明な true-peak リミッタではない）: L/R 連動の速い peak compressor + 最後に**有界の**ソフトクリップ曲線（出力は数学的に天井を越えない = サンプルピークの保証）。look-ahead なしなので立ち上がりの一瞬はクリップ曲線が受ける — 声は loudnorm 済みで、ここに届くのは稀。opt: look-ahead 1.5 ms（gain の先読み）+ 4× の true-peak 検出 | `ceiling_db: -1, release_ms: 80, lookahead_ms: 0` | ~12（look-ahead 時 ~40） | 0（opt 1.5 ms） |
| `deesser` | **ダイナミック high-shelf**: 検出は HP（既定 6.5 kHz）帯域の包絡、効かせるのは high-shelf biquad 1 段の gain（0〜-`max_db`）。帯域を分けて足し戻さないので、圧縮中も位相の打ち消しや色付きが出ない（分割＋再合成は `1+(g-1)H_HP` の位相で色付く） | `freq: 6500, threshold_db: -30, ratio: 3, max_db: -8` | ~20 | 0 |
| `exciter` | 高域（`freq` 以上）だけを tanh で倍音化し、もう一度 HP して少量足す（濁らない「空気」）。非線形部は saturate と同じ ADAA | `freq: 4500, drive: 2, mix: 0.08` | ~25 | 0 |
| `saturate` | tanh / 非対称 soft-clip、レベル補償。**1 次 ADAA**（antiderivative antialiasing: tanh の原始関数 log cosh の差分で折り返しを ~20 dB 減らす。代わりに 0.5 サンプルの遅れ）+ 非対称時は DC ブロッカ。エイリアスはスイープで実測して §9 の基準に入れる。足りなければ v2 で 2× オーバーサンプル（halfband IIR、遅延は数サンプル） | `drive: 1.5, mix: 1, asym: 0` | ~15 | 0.5 サンプル |
| `chorus` | 1–4 声の変調ディレイ（3 次 Hermite 補間）、LFO は sin 近似、声ごとに位相と速度をずらし L/R に配置。dry はそのまま | `voices: 2, rate_hz: 0.25, depth_ms: 2.5, base_ms: 12, mix: 0.3, spread: 1` | ~12×声数 | 0（wet のみ遅れ） |
| `doubler` | chorus の特殊形: 1 声、ランダム（平滑化ノイズ）で 8–25 ms と微小ピッチ揺れ | `delay_ms: 15, wobble: 0.3, mix: 0.25` | ~15 | 0 |
| `widen` | M/S 幅（`M=(L+R)/2, S=(L-R)/2`、`width` は S だけに掛かる。モノに畳むと side が消えるだけ）+ 任意の Haas は **side にだけ**入れる（モノ和が変わらない）。mono の声には side が無いので widen 単体では広がらない — side を作るのは chorus / reverb。widen の後ろに非線形ノードを置くと対称性が崩れるので validate で warning | `width: 1.0, haas_ms: 0, bass_mono_hz: 150` | ~6 | 0 |
| `reverb` | **8 線 FDN**。帰還行列は正規化 Householder `A = I - (2/N)·11ᵀ`（直交）。線 i の帰還ゲイン `gᵢ = 10^(-3·Dᵢ/(fs·T60))` に、減衰用 1 次 LPF（DC ゲイン 1、全帯域で \|H\|≤1）を掛ける → ループゲインは全周波数で < 1。遅延長はゆっくり変調してメタリック感を消す（深さ ≤ 8 サンプル、速さ ≤ 1 Hz、**変調線は線形補間**（Hermite はオーバーシュートでループゲイン > 1 になり得る））。入力に 4 段 allpass 拡散。全 param の端の組み合わせで安定性テスト | `decay_s: 1.2, size: 1, damp_hz: 6000, predelay_ms: 10, diffusion: 0.7, mod: 0.2, width: 1` | ~70 | 0（wet のみ predelay） |
| `shimmer` | 「`hp` → `pitch_shift` → `reverb`」を束ねた糖衣（reverb の params も書ける）。pitch shifter は 2 タップ遅延線＋sin² クロスフェード（Doppler 型、窓 50 ms）。周期的なうねりと立ち上がりのにじみが出る — wet 専用・低い mix で使う前提で、残響が隠す。上げる時は読み出しが速いので**前段に LPF（fs/(2·比) 付近）を入れて折り返しを防ぐ**。**v1 は帰還なし**（音が昇り続ける版は v2: 帰還 ≤ 0.5、ループ内にリミッタ、+12 / +19 / -12 半音で安定性テスト） | `semitones: 12, mix: 0.2, hp_hz: 600` + reverb の params | ~100 | 0（wet のみ ~25 ms） |
| `ringmod` | 低い周波数（20–80 Hz）の sin を**少量**かける（`1 - mix + mix·sin`）。電子っぽい粒。高い周波数はロボ声になるので range で 200 Hz までに制限 | `freq: 50, mix: 0.05` | ~4 | 0 |
| `formant` | **subtle** なフォルマント移動: v1 は 3 本の peak を動かす「疑似フォルマント EQ」（声道長を ±5% 程度に感じさせる）。本物の LPC / PSOLA は v2 | `shift: 0.0`（-1..1） | ~20 | 0 |
| `radio` | 帯域制限（HP 2 次 + LP 2 次）+ 中域の山 + 軽い saturate（ADAA）のプリセット化ノード。電話ほど狭くしない | `low_hz: 220, high_hz: 6000, presence_hz: 1400, presence_db: 3, drive: 1.8, mix: 0.6` | ~30 | 0.5 サンプル |
| `bitcrush` | ビット深度と sample-and-hold の降サンプル、**ごく少量**を wet で足す。ディザ（TPDF）付きで耳障りなトーンを避ける | `bits: 12, downsample: 1, mix: 0.05` | ~6 | 0 |
| `gate` | ヒステリシス付き（open/close 閾値）、hold、ゆっくり閉じる（range で完全に 0 にしない） | `open_db: -50, close_db: -56, hold_ms: 60, release_ms: 120, range_db: -24` | ~8 | 0 |
| `duck` | サイドチェーン圧縮: `key` の bus（例: voice）の包絡でこの経路を下げる | `key: voice, depth_db: -9, attack_ms: 5, release_ms: 250` | ~10 | 0 |
| `gain` / `pan` / `mix` | 基本部品（`$params` の受け口として使う） | `db: 0` / `pos: 0` / `amount: 1` | ~1 | 0 |
| `meter` | 解析ノード（§6）。音は通過のまま | `level, onset, centroid, tail` | ~10（重心は 256 点 FFT を 2 ブロックに 1 回） | 0 |

実装メモ:
- **実行中に変えてよい param と、変えられない param を分ける**。連続値（周波数・gain・mix・時間）は
  `runtime_mutable` にでき、制御レートで補間 → 係数を作り直す（biquad は TDF-II transposed なので
  滑らかな係数変化で安定。周波数は対数で補間）。遅延時間は**変化速度を制限**（1 サンプルあたり 1 サンプル未満）。
  離散値（フィルタの種類・声の数・ビット数・帯域の段数）は**compile 時だけ**（実行中に変えたい時は
  プリセットを2つ作ってクロスフェード）。
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

analysis: { voice: true, level: true, onset: true, centroid: true, tail: true, rate_hz: 60 }
```

規則:
- **chain / buses / master の3段だけ**。任意の DAG は v1 では書けない（読みやすさ・検証・
  コスト見積もりのため）。bus から bus への send は 1 段まで（ループ禁止を型で保証）。
- `mix.amount` は master で「この Sound 全体の効き」を決める特別扱い: Graph は voice_base の後の信号を
  dry としてコピーしておき、`out = dry·(1-a) + wet·a` を**線形**で混ぜる（dry と wet は同じ声で相関が高いので、
  等パワーにすると中間で +3 dB 膨らむ）。a=0 は「色付けなしの声（レベル整えと limiter は残る）」で、**素通しではない**。
- **素通し（bypass）は別の操作**: `fa_bypass(inst, 1)` は limiter も含めて全部を外す。切替は 20 ms のフェードで、
  bypass 前に鳴っていた尾は wet 側だけで減衰させて流し切る。解析は bypass 中も動く。
- **継承（`extends`）の規則**: 親の chain が子の chain の前に展開される。params・buses は名前でマージし、
  子が同名を書いたら子が勝つ。master は**子が書いた時だけ置き換え**（書かなければ親のもの）。io は子の宣言が勝つ
  （mono→stereo へ広げるのは可、逆は拒否）。`extends` の深さは 2 まで、循環は拒否。
- 資源の上限（validate が拒否）: ノード 64 個、bus 8 本、1 ノードの帯域 8 段、YAML 64 KB、param 64 個。
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
crossfade: { duration: 0.3, ease: ease_in_out }   # 声は線形クロスフェード、旧プリセットの尾は流し切る（下の規則）
```

Graph はバンクの全プリセットを compile 時に構築し（メモリは §3.6 のとおり合計 2 MB 程度）、
`fa_select(inst, "fluent")`（RT 側は番号の `fa_select_idx`）で切り替える。

切替の規則:
- 新プリセットの声（dry + chain）は 300 ms の**線形**クロスフェードで入れ替える（同じ声なので相関が高い）。
- 旧プリセットは**入力を止めて**走り続け、bus の尾だけが自然に減衰する（最長 2 s、または `tail` が -60 dB 未満で停止）。
- 同時に走るのは**最大 2 つ**。尾の途中でさらに切り替えたら、一番古い尾を 50 ms でフェードアウトして止める。
- 最後の保護（limiter）は**バンクの合計の後**に1つだけ置く（各プリセットの master の limiter は、バンクの中では外す）。

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
int fa_set_param_idx(FaInstance*, int index, float v);          // RT 安全版（次の制御刻みで適用）
int fa_set_param_at(FaInstance*, int index, float v, uint64_t frame);  // サンプル時刻付き（決定的）
int fa_select(FaInstance*, const char* preset);                 // バンク内の切替（クロスフェード）
int fa_preset_index(FaInstance*, const char* preset);
int fa_select_idx(FaInstance*, int index);                      // RT 安全版
int fa_bypass(FaInstance*, int on);                             // 完全な素通し（20 ms フェード、尾は流し切る）
void fa_reset(FaInstance*);                                     // 状態を捨てる（尾も消える。無音時に使う）

// 解析（§6）と診断
const FaAnalysis* fa_analysis(FaInstance*);        // 最新の解析（版番号つき固定レイアウト）
int          fa_diagnostics_count(FaInstance*);    // RT 側で数えた警告（遅着イベント・NaN reset・キュー溢れ）。数値だけ
const char*  fa_diagnostics(FaInstance*);          // 直前の setter が 0 を返した理由（非 RT。読むと空になる）
const char*  fa_drain_events(FaInstance*);         // onset / tail_end などのイベント（非 RT で読む）
```

C++ 側（`namespace fluent_audio`）:

```cpp
auto parsed   = fluent_audio::sound::parse(yaml);       // 誤りは全部ここで拒否
fluent_audio::CompileOptions opt;  opt.sample_rate = 48000;  opt.max_block = 128;
auto compiled = fluent_audio::sound::compile(parsed.doc, opt);
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
| `voice` | **声の入力**（voice_base の後・色付けの前）の RMS 包絡（10 ms 窓）を dBFS → 0–1（-60 dB=0、-6 dB=1）、attack 5 ms / release 120 ms。UI 音やリバーブを含まない | **口**（今の level の置き換え）。尾でパクパクしない |
| `level` | **出口**（master の後）の同じ包絡。UI 音・残響を含む | 発光・オーラの大きさ |
| `peak` | ブロック内の最大絶対値 | クリップ監視 |
| `onset` | 包絡の対数微分がしきい値を越えた瞬間（音節の頭）。不応期 60 ms | 口のパク、粒の発生 |
| `centroid` | スペクトル重心 Hz（256 点 FFT、2 ブロックごと）→ 0–1（300 Hz–5 kHz の対数） | 明るさ → 顔の色温度・きらめき |
| `tail` | 出口が -60 dBFS を下回ってから 100 ms 続くまで 1（ヒステリシス: -54 dB で再び 1） | 「まだ自分の音が鳴っている」の目安（§13.4）。エコーキャンセルではない |

- 「声が出ているか（VAD）」の判定は持たない（`voice` の大きさで足りる。本当の VAD は fv_audio_vad の仕事）。
- 版番号つき固定レイアウト `struct FaAnalysis { uint32 version; uint32 size; uint64 frame; float voice, level, peak, onset, centroid, tail; }`。
  `frame` はその値を計算した最後のサンプルの絶対番号。`peak` だけはブロックの区切りに依存するので
  チャンク不変の対象外と明記する。
- 送る頻度は画面に合わせて ~60 Hz。**口と音を合わせる**: worklet が `frame` を付けて送り、メインスレッドは
  `AudioContext.getOutputTimestamp()`（contextTime と performanceTime の対応）で「そのサンプルが
  スピーカーから出る時刻」に変換し、描画時刻に一番近い値を使う（遅延の二重計上をしない）。
  getOutputTimestamp が無い / 0 を返す端末では `outputLatency`、それも無ければ `baseLatency` を使う
  （これは較正していない目安で、端末ごとに目で合わせる値を設定に持つ）。
- fluent_scene の `$params.level` / `speak` に直結できる（Liquid の level と同じ意味）。

---

## 7. look ごとのプリセット（初期値）

全プリセット共通の前段 `voice_base`: `eq hp 80 Hz` → `compressor（-24 dB, 2:1, 10/150 ms, +2 dB）`
→ `deesser（6.5 kHz, -32 dB, 3:1）`。最後は `limiter -1 dBFS`。**音量は全 look で揃える**
（プリセットごとに `makeup` を測って固定し、golden の loudness テストで ±1 LU を守る）。

| look | 狙い | 中身（chain / buses） | 試作 |
|---|---|---|---|
| **kepler** | いつもの声を「少しだけ良く」。きれい＋空気 | peak 3.2 kHz +1.5 / highshelf 10 kHz +2.5 / exciter 4.5 kHz 0.08 / 小部屋 reverb 0.5 s（wet は dry 比 -31 dB*） | ✓ |
| **luna** | ルナ自身の声（Qwen3）。**色付けなし** | voice_base だけ（低域カット・レベル整え・歯擦音）+ limiter。キャラの効果は掛けない | – |
| **fluent** | 柔らかい液体。きらめき | 上の §5.1 の例そのもの（コーラス 3 声・幅 1.3・室 1.6 s（-22 dB*）・1 オクターブ上のシマー（-22 dB*）） | ✓ |
| **kitt** | 車載コンピュータ。無線の帯域＋軽い歪み | radio（220 Hz–6 kHz、1.4 kHz +3）/ saturate 1.8 mix 0.6 / ringmod 55 Hz 0.05 / 車内の短い反射 0.35 s | ✓ |
| **hal** | 落ち着いた、暖かい、近い。冷たくしない | lowshelf 200 Hz +1.5 / peak 3 kHz -1（耳当たりを丸く）/ highshelf 9 kHz -1 / doubler なし / 小部屋 0.7 s を -26 dB / compressor をもう少し深く（3:1）で「近接効果」 | – |
| **gargantua** | 深宇宙。遠くまで伸びる暗い残響を**薄く** | lowshelf 160 Hz +2 / 暗い大空間 reverb 5.5 s（damp 3.5 kHz、predelay 60 ms、-17 dB*）/ 1 オクターブ下のシマー 6 s を薄く | ✓ |
| **aurora** | 夜空の光のカーテン。高く透明 | highshelf 8 kHz +3 / chorus 2 声 0.12 Hz / シマー +12（オクターブ）と +19（その 5 度上）を同時に、オクターブを強めに薄く / reverb 2.2 s 明るめ（damp 10 kHz） | – |
| **system** / **comic** | 未定。当面 kepler と同じ | `extends: kepler` | – |

\* dB は send の値ではなく、**試作で測った wet のエネルギーの dry 比**（send 0.05 × reverb の利得 = -31 dB 等）。
本番では YAML に `wet_db:` で書けるようにし、reverb の内部利得の違いに左右されないようにする（M2）。

試作（✓）は numpy の sketch で作った（`sketch/render.py`、§12）。試聴: `media/faudio.view.json`。
各 YAML は `presets/*.yaml` に置いた（v1alpha1 の書き方の見本を兼ねる）。

look 以外の param（全プリセットが持つ）:
`amount`（効きの強さ 0–1、設定画面のスライダーにする）、`speak` / `think`（顔と同じ気分の重み。voice_base で宣言。
例: fluent は think 中に shimmer を少し上げる、程度の**ごく小さな**連動に留める。v1 のプリセットではまだどこにも束縛しない）。

---

## 8. 「心地よい」を守る規則（品質の線引き）

fluent_scene のデザインリンターに相当する **サウンドリンター** を validate に入れる:

1. **明瞭度（目安）**: 声の帯域（300 Hz–4 kHz）で、試験用の朗読を通した時の wet（bus の合計）のエネルギーが
   dry より 10 dB 以上小さい。STI の代わりにはならない経験則で、warning に留める。
2. **耳当たり**: 2–5 kHz を合計で +4 dB 以上持ち上げたら warning（刺さる帯域）。
3. **ピッチ**: dry 経路にピッチを変えるノードを置いたら warning（声のキャラが変わる。`allow_pitch: true`）。
4. **モノ互換**: L+R を足した時の loudness 低下が 1.5 LU 以内（Tesla の車内・iPhone のスピーカーは実質モノ）。
5. **音量**: 試験用の朗読コーパス（男女・速さ違い 10 本）で、プリセット間の integrated loudness 差 ±1 LU
   （look を変えて声が大きくなる/小さくなるのを防ぐ。任意の声で保証するものではない）。
6. **遅延**: `latency_samples > 0` は warning（D3）。
7. **ring-mod / bitcrush の mix 上限**: range で 0.15 まで（ギミック化の防止）。

---

## 9. テスト

| 種類 | 中身 | 合格基準 |
|---|---|---|
| **golden（オフライン）** | 固定入力（インパルス、対数スイープ、ピンクノイズのバースト、無音→音→無音、CC0 の朗読）× 全ノード × 全プリセット × {44.1k, 48k} → `tests/golden/*.wav`（float32） | **null テスト**: 差分の RMS ≤ -100 dBFS、最大 ≤ -90 dBFS（コンパイラの版替えを許すための許容。同じビルドの native↔wasm は下のとおり許容なし）。golden の更新は fluent_scene と同じく理由付きのコミットだけ。加えて周波数特性・減衰時間（RT60）・エイリアス量を数値で検査 |
| **native ↔ wasm** | 同じ入力 PCM・SR・初期状態・時刻付きイベント列を native（x86-64 / aarch64）と wasm（node で実行）で処理 | **ビット一致**（D8）。ここは許容差なし — ずれたら §3.5 の数値規則の破れとしてバグ扱い |
| **チャンク不変** | 同じ入力と**同じ時刻付きイベント列**を 128 固定 / 1〜512 のランダム長で process | ビット一致（D4。`peak` 以外の解析値も） |
| **RT 安全** | `operator new` / `malloc` のフック、mutex のフック | process 中 0 回 |
| **denormal** | インパルスの後に無音 120 s | 全状態変数が有限時間で正確に 0 になる（値を直接検査）。処理時間はベンチ側で別に見る |
| **安定性** | 全ノードの param の端の組み合わせ（FDN の decay 最大 × 変調最大 等）で 60 s | 出力が有界、尾が減衰して 0 に着く |
| **param ファズ** | ランダムな時刻に全 param をランダム値で setParam | NaN / Inf なし。クリック検出 = 同じ入力を「param を滑らかに動かした参照」と比べ、差分の 8 kHz 以上のエネルギーが一瞬だけ跳ねないこと |
| **ブラウザ実機** | ベンチページで Chrome / Safari / Tesla / iPhone。GC を起こすページ負荷を掛けながら 10 分再生 | 途切れ 0（`AudioContext` の underrun 統計がある所はそれ、無ければ録音して検出） |
| **品質（§8）** | loudness（ITU-R BS.1770 を自前実装）、モノ互換、明瞭度 | §8 の数値 |
| **ベンチ** | ノード別・プリセット別の ns / block。farm3（x86）、Mac、node の wasm、ブラウザのベンチページ（Chrome / Safari / Tesla / iPhone） | §3.6 の 5% 予算。CI で 20% 以上の悪化は fail |
| **主観（記録）** | 新プリセットは A/B（dry との）を試聴ページに追加し、オーナーの OK を記録 | — |

TTS の声のライセンス: AivisSpeech のモデルは利用規約がモデルごとに違う。golden に入れる
声は **自分たちで録る CC0 の朗読**か、規約で再配布が許されたものに限る（Kepler の
声のクリップはローカルの主観テストにだけ使い、リポジトリに入れない）。試験音源の出典と許諾は THIRD_PARTY.md に記録する。

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

補足:
- 「依存ゼロ」は **DSP コアの話**。native の I/O（miniaudio）、Python（nanobind）、ビルド（Emscripten）は周辺の依存。
- 「参考にする」は**論文・公開された解説を読んで自分で書く**意味で、コードは写さない。写した・改変した物が出たら
  上流のリビジョン・ライセンス文・必要な表記を THIRD_PARTY.md に記録する。同梱する物は版を固定する。
- Faust は「生成コード＝GPL」ではない（元の .dsp と使ったライブラリ・architecture ファイルのライセンスを継ぐ）。
  ただし標準ライブラリに GPL / LGPL / STK が混在し、監査の手間に見合わないので使わない、という判断。
- 自前実装は著作権の問題を避けるが、特許の確認にはならない。v1 のアルゴリズム（biquad、FDN、Doppler 型ピッチ、
  ADAA、M/S）はいずれも古く公知だが、v2 で新しい手法（formant 保持ピッチ等）を入れる時は個別に確認する。

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

試作と本書の違い: de-esser は分割帯域（本書はダイナミック shelf）、saturate は ADAA なし、
shimmer の前段 LPF なし、limiter は簡易。どれも聞き比べの方向を示すには十分だが、golden の元にはしない。

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
- `level()` は worklet から届いた最新の `analysis.voice`（口用、§6）を返す（同じ 0–1 の意味。顔側は変更なし）。
  余裕があれば `level`（出口）/ `onset` / `centroid` も faceWin のメッセージに足す（各顔が使うかは顔ごとに後で）。
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
  ページはそれを見たら**その文だけ** worklet を `fa_bypass`（limiter も外す完全な素通し）にする。
  `/api/tts` は同一オリジンなので CORS の expose は不要（別オリジンから取る時は `Access-Control-Expose-Headers` に足す）。
  掛けた文と掛けていない文が続く時は、bypass の切替を文の境目（無音）で行い、前の文の尾は流し切る。

### 13.4 気をつけること

- **自分の声を自分で聞く問題**: agent-ext.js はマイクの言葉を「自分の声が鳴っている間」は捨てている。
  リバーブの尾（gargantua で最大 ~1.5 s 聞こえる）の間もマイクに声が入り得るので、
  「自分の音が鳴っている」の判定を `analysis.tail` が落ちるまで延ばす（ヒステリシス付き、上限 1.5 s）。
  これはエコーキャンセルではなく目安。ブラウザの echoCancellation が尾をどこまで消すかは実機で確かめる。
- **iPhone Safari**: AudioContext は tap で resume（tts.js が既にやっている）。電話・Siri・イヤホン抜けで
  `interrupted` になり得るので、状態変化を見て次の tap で resume する。消音スイッチで Web Audio が黙る件は
  `navigator.audioSession`（ある時だけ。機能検出する）で `playback` にすれば鳴る — するかはオーナー判断（§15）。
  `audioWorklet.addModule` は secure context が要る（Kepler は https / localhost で OK）。
  **CSP**: worklet のスクリプトと wasm を同一オリジンから出し、CSP を足す時は `script-src` に `'wasm-unsafe-eval'` が要る。
  Kepler の顔は iframe なので、音を iframe の中で鳴らす時は `allow="autoplay"` が要る（今は親ページで鳴らすので不要）。
- **Tesla**: Chromium だが版と CPU は車種・ソフトの版で違う。対応は「実機で測った版の表」で管理し、
  ベンチページで 5% 予算を満たした版だけ既定オンにする。
- **出力先の変化**（AirPods の接続など）で sampleRate が変わることは無い（AudioContext の SR は固定）が、
  出力遅延は変わる → 口合わせは毎回 getOutputTimestamp から計算する（§6）。

---

## 14. マイルストーン

| M | 中身 | 完了の定義 |
|---|---|---|
| **M1** | C++ コア（Graph、param + animate、時刻付きイベント、制御レート、denormal / NaN 対策、自前の数学近似）+ **5 ノード**（eq, compressor, exciter, reverb, limiter）+ Sound YAML（chain / buses / master / extends）+ wasm worklet（握手・フォールバック込み）+ Kepler の tts.js に機能フラグ付きで接続 + `voice` / `level` 解析で顔を駆動。M1 の kepler プリセットは de-esser 抜き（compressor で代用、TTS の歯擦音は穏やか） | golden（native）と native↔wasm ビット一致と RT 安全テストが緑。iPhone / Mac / Tesla で kepler プリセットが鳴り、顔が動く |
| **M2** | 残りのノード（deesser, saturate, chorus, doubler, widen, shimmer, ringmod, formant, radio, bitcrush, gate, duck, meter 全項目）+ **全 look のプリセットとバンク切替（クロスフェード・尾の流し切り）**+ bypass + サウンドリンター + UI 音のダッキング + `wet_db` | 全プリセットの golden と §8 の品質テストが緑。オーナーが試聴ページで各 look を OK |
| **M3** | native の `fa-render` / `fa-play`（miniaudio）+ server.py の `fa_preset` + `X-FA-Applied` + G2 / ファイル書き出しの経路 | 同じ PCM・同じ SR を入れた時、サーバとブラウザ（wasm）の出力がビット一致。実際の経路どうし（変換器が違う）は loudness ±0.2 LU・スペクトル ±0.5 dB で比べる |
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

---

## 付録 A. レビューの記録

- 2026-10-19 codex（gpt-6-astra）に本書の初稿をレビューさせ、28 項目の指摘を受けた。
  採り入れたもの: worklet でのコンパイルを避けて Module を渡す・握手とフォールバック（§3.2）、
  postMessage の GC を「ゼロ」と言わない（§3.2）、時刻付き param イベントでチャンク不変を定義（§3.5）、
  param スロットとイベントキューの分離（§3.5）、denormal の閾値と flush 位置の明確化（§3.5）、
  ビット一致の数値規則と対象アーキテクチャ（§3.5・§9）、「遅延 0」を追加遅延に限定（§3.6）、
  reverb のメモリ見積もりの誤り（6 万→6 千サンプル）の修正（§3.6）、de-esser をダイナミック shelf に（§4）、
  saturate の ADAA と DC ブロッカ（§4）、FDN の安定条件と変調線の線形補間（§4）、shimmer の帰還を v2 へ・
  前段 LPF（§4）、limiter を「安全リミッタ」と呼ぶ（§4）、mix を線形に・bypass を別操作に（§5.1）、
  extends の規則と資源上限（§5.1）、バンク切替の尾の扱いと limiter をバンクの後に（§5.2）、
  口用の `voice` と出口の `level` の分離・getOutputTimestamp での口合わせ（§6）、
  品質テストの言い過ぎの修正（§8・§9）、ライセンスの但し書き（§10）、Safari / Tesla / CSP の注意（§13.4）、
  M1 のノード構成とプリセットの不一致の解消（§14）。
- 採らなかったもの: なし（言い回しの程度の差はあるが、全項目に対応した）。
