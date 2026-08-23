# Phase 4: 計測と記録

計測は実機・release 相当条件を明記(CPU 周波数、最適化レベル、interpreter 種別)。

1. ホスト API 呼び出しコスト: wasm 内ループから `hostapi_now_ms` を N 回呼び、
   `esp_cpu_get_cycle_count()` でサイクル数/回を算出。ネイティブ直呼びと比較。
2. ジッタ: tick タスクの起床間隔と `app_tick()` 実行時間を `esp_timer_get_time()` で
   数千サンプル収集し、min/avg/max/分布を記録。
3. メモリ: flash 増分、WAMR プール消費、instantiate 後 free heap、
   `wasm_runtime_dump_mem_consumption` の出力。
4. 所見: interpreter のままで音楽アプリロジックに足りるか、AOT へ進む判断材料を書く。

## Phase 4 実施記録 (2026-07-05) — 完了

計測結果と所見は本ファイル末尾の「計測結果詳細」に集約(条件: -Og, 160MHz, fast-interp)。
ヘッドライン: host→wasm 起動 15.8µs、wasm→host 越境 ~2.5〜3.7µs/回、
100ms tick の定常ジッタ +10µs 以内、WAMR 実消費 ~27KB(プール 128KB は
64KB へ縮小可)、interp ループ ~215 cycles/iter。
**結論: interpreter のままロジック層には十分。AOT は信号処理を wasm に
持ち込む段になってから。**

実装メモ:

- `wasm-apps/bench/`: 計測用 wasm(`bench_empty`/`bench_hostcall`)。
  `bench_empty` は LCG 形式(`acc*1664525+i`)——単純な `acc+=i` だと LLVM が
  閉形式(等差数列の和)に畳んでループが消えるため。
- demo 起動時に bench が自動実行され、tick ループが最初の 1000 回の
  起床間隔/実行時間を収集して統計をログする(常設。計測バッファ 8KB)。
- tick 駆動は `vTaskDelay` → **`vTaskDelayUntil`(絶対時刻基準)に変更**。
- `wasm_runtime_dump_mem_consumption` は `WAMR_ENABLE_MEMORY_PROFILING=y`
  ビルドのみ存在(コードは `#if` ガード済み)。WAMR 2.4.0 では同フラグが
  `-Werror=dangling-pointer` でビルド失敗するため、`src/CMakeLists.txt` で
  WAMR コンポーネントにのみ `-Wno-dangling-pointer` を付与して回避。

## 未確定・要実測事項(推測で進めない)

- 初期化後の内部 SRAM 空きヒープ実測値(Phase 1 で取得)。
- WAMR 2.4.0 component の実フットプリント(flash/RAM)と menuconfig 推奨値。
- fast interpreter(既定, メモリ2倍・速い)と classic interpreter の選択 → Phase 1 は
  既定で開始し、メモリが厳しければ classic を試す。
- I2S クリック音の初回発音レイテンシ(Phase 2 で確認)。

## 計測結果詳細

計測日: 2026-07-05

### 計測条件

| 項目 | 値 |
|---|---|
| ボード | Waveshare ESP32-S3-Touch-LCD-2.8(ESP32-S3, 16MB Flash / 8MB PSRAM 搭載) |
| CPU | 160 MHz(`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160`) |
| PSRAM | 無効(すべて内部 SRAM 512KB) |
| 最適化 | -Og(`CONFIG_COMPILER_OPTIMIZATION_DEBUG`) |
| ESP-IDF | v5.5.1 |
| WAMR | 2.4.0~1(ESP Component Registry)、**fast interpreter**、libc builtin のみ(AOT/WASI/pthread lib/App Framework 無効) |
| WAMR ヒープ | `Alloc_With_Pool` 静的 128KB、instantiate stack/heap = 8KB/8KB、exec_env stack 8KB |
| .wasm | Rust 1.95 no_std / wasm32-unknown-unknown、opt-level=z + LTO + strip、`-zstack-size=8192` |
| 計測コード | `wasm-apps/bench/`(計測1)+ `wasm_runtime.cpp` の tick ループ計装(計測2) |

計測値は起動ごとのシリアルログから採取。-Og かつ 160MHz なので、
リリース最適化(-O2)や 240MHz 化でネイティブ側・WAMR とも改善余地がある。

### 1. ホスト API 呼び出しコスト

`bench.wasm` のループ(N=100,000)を `esp_cpu_get_cycle_count()` で外側から計測。

| 項目 | cycles | 時間 @160MHz |
|---|---|---|
| host→wasm 関数起動(`wasm_runtime_call_wasm`、引数1個) | ~2,530 / call | **15.8 µs** |
| wasm 内ループ 1 周(LCG: mul+add+比較+分岐 ≒ wasm 6 命令程度) | ~215 / iter | 1.34 µs |
| wasm→host 呼び出し(`hostapi_now_ms`、実測差分法) | ~630〜860 / call | **4〜5.4 µs** |
| (参考)同処理のネイティブ直呼び | ~269 / call | 1.68 µs |

- wasm→host の値は `(bench_hostcall(N) − bench_empty(N)) / N`。ループ本体の
  差(hostcall 側は LCG なし)があるため幅を持たせて記載。**サンドボックス境界の
  越境オーバーヘッドは実質 ~400〜600 cycles(2.5〜3.7 µs)**。
- 所見: ホスト API を「フレーム単位・イベント単位」で呼ぶ設計なら十分安い。
  サンプル単位(44.1kHz)で越境する設計は不可(1 サンプル 22.7 µs に対し
  呼び出しだけで 4µs超)——オーディオ生成をネイティブ側に置く方針の妥当性を裏付け。

### 2. タイミングジッタ(tick 駆動)

ホスト側 tick スレッド(pthread, prio 5)が `vTaskDelayUntil` で 100ms 周期に
`app_tick()` を呼ぶ。起床間隔と実行時間を 1000 サンプル収集(2 回のブートで再現確認)。
LVGL 描画タスク・クリック音 I2S 出力が同時に動作している実運用条件。

| 指標 | min | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|
| 起床間隔 (µs, 目標 100,000) | 90,949 | 99,990 | 100,000 | 100,005 | 100,005 | **100,010** |
| `app_tick` 実行時間 (µs) | 21 | 240 | 21 | 917 | 5,104 | 5,120 |

- 間隔の min(≈91ms)は、直前の tick が長かった際に `vTaskDelayUntil` が
  位相を維持するため詰めたもの(仕様どおりの挙動)。**定常ジッタは +10µs 以内**。
- 実行時間の二極分布: 通常 tick(何もしない)= 21µs、1 秒境界の tick
  (draw_text + play_click)= 1〜5ms。上限 5.1ms は LVGL ロック競合+
  I2S 書き込み由来で、100ms 周期に対し十分小さい。
- 所見: FreeRTOS tick(10ms 分解能)上で `vTaskDelayUntil` を使う限り、
  周期駆動の UI/シーケンサ用途(10〜100ms 周期)には**ジッタは問題にならない**。
  1ms 未満の粒度が必要になったら esp_timer / ハードウェアタイマ駆動を検討。

### 3. メモリ

#### RAM(実測)

| 項目 | 値 |
|---|---|
| wasm モード起動直後の free heap(Display/LVGL/audio 初期化前) | 226,364 bytes |
| 全初期化後(Display+LVGL+I2S+WAMR+demo module+計測バッファ 8KB)free heap | **47,668 bytes** |
| WAMR 静的プール(BSS) | 131,072 bytes |
| demo モジュールロード時の malloc 消費(プール外, fast-interp の再コンパイル領域等) | ~18.7 KB |

#### WAMR 内部消費(`wasm_runtime_dump_mem_consumption`, MEMORY_PROFILING=y ビルド)

| 項目 | 値 |
|---|---|
| module(バイトコード構造体等) | 1,839 bytes |
| module inst + app heap(8KB)+ linear memory | ~17.4 KB |
| exec env(uint 8KB スタック込み) | 8,264 bytes |
| **合計(モジュール+インスタンス+exec env)** | **27,471 bytes** |
| interpreter スタック実使用(高水位) | **152 bytes** |
| app heap 実使用 | 0 bytes(no_std・アロケータ不使用のため) |

- ダンプ中の一部フィールド(module inst total 等)は異常値を出力する
  (プロファイリング実装の表示バグ、集計行は妥当)。
- 所見: demo 規模のアプリでは**プール 128KB に対し実消費 ~27KB**。
  プールは 64KB まで縮小可能な見込みで、その場合 free heap は 110KB 程度まで回復。
  wasm 側スタックも 8KB 割り当てに対し実使用 152B と余裕が大きい。

#### Flash

| 項目 | 値 |
|---|---|
| WAMR ランタイム(トリム構成) | ~67 KB(.text 66.4KB + DIRAM ~2KB) |
| アプリバイナリ: wasm デモモード | 710 KB(1MB app パーティションの 68%) |
| アプリバイナリ: MP3 デモモード(WAMR リンクなし相当) | 769 KB(基準 741KB +28KB) |
| hello.wasm / demo.wasm / bench.wasm | 106 / 730 / 244 bytes |

### 4. 所見と次の判断材料

1. **interpreter のままで音楽アプリの「ロジック層」には足りる。**
   fast-interp は ~215 cycles/ループ(ネイティブ比おそらく 20〜40 倍遅)だが、
   tick 駆動のシーケンサ・UI ロジックは 1 tick あたり数百〜数千 wasm 命令程度で
   1ms 未満に収まる。100ms 周期に対し余裕は 2 桁ある。
2. **ホスト API 境界コスト(~2.5〜3.7µs/回)は API 設計で吸収する。**
   「音を出す」「矩形を描く」単位の粗粒度 API なら問題なし。バッファを渡す
   バルク API(例: ノートイベント配列)にすれば越境回数はさらに減らせる。
3. **AOT へ進む条件**: wasm 内で信号処理・多量のイベント処理(例: 1 tick に
   数十万命令)をやりたくなった時。AOT は interpreter 比 5〜10 倍の高速化が
   見込める一方、.wasm 配信ではなく .aot 配信(ターゲット依存)になるため、
   「配信フォーマットの可搬性」とのトレードオフを Phase 5 以降で検討する。
4. **メモリ方針**: プール 64KB への縮小で内部 SRAM のまま余裕が作れる。
   複数アプリ同時実行や大きな linear memory が必要になったら PSRAM(8MB,
   現状未使用)へプールを移す(レイテンシ増の再計測が条件)。
5. 計測は -Og / 160MHz。リリース条件(-O2 / 240MHz)では全数値が改善する
   方向であり、本結果は保守側の見積もり。

### 再現手順

- ベンチ: `CONFIG_MIDIBOX_WASM_DEMO=y` でビルドすると起動時に bench が走り、
  約 100 秒後に jitter 統計がシリアルに出る。
- メモリダンプ: menuconfig で `WAMR_ENABLE_MEMORY_PROFILING=y` にして再ビルド
  (WAMR 2.4.0 では `-Wno-dangling-pointer` が必要 → `src/CMakeLists.txt` 参照)。
