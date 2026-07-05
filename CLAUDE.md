# MidiAppBox — WASM PoC 計画と調査記録

ESP32-S3-Touch-LCD-2.8 (Waveshare) ベースの音楽デバイスファームウェア。
「サンドボックス化された WASM アプリを組込みデバイスに配信する音楽プラットフォーム」の
成立性検証 PoC を進行中。

## 開発の進め方(このリポジトリでの作業ルール)

- 「小さいターゲットを定めて、テストし、次を計画する」の反復。各フェーズはビルドが通り
  コミット可能な粒度を保つ。
- ビルド/フラッシュは tmux pane 1(Docker コンテナ内, `idf.py build` / `idf.py flash`)。
  `tmux send-keys -t .1 '<cmd>' Enter` で送信、`tmux capture-pane -p -t .1` で確認。
- Phase 完了まで、ビルド・フラッシュ・モニタ確認を含めて確認なしで自律的に進めてよい。各ステップの結果はログとしてCLAUDE.mdか作業メモに残すこと。
- 依存追加は最小限。追加時は本ファイル末尾「依存の記録」に理由を残す。
- 既存の MP3 再生デモは壊さない(起動モードで分岐)。

## アーキテクチャ方針(決定済み)

- タイミングクリティカル層(オーディオ出力、描画ドライバ、将来の FM 音源)はネイティブ
  ホスト側。WASM アプリはロジックのみを持ち、ホスト API を叩く。
- WASM アプリは Rust / `wasm32-unknown-unknown`(WASI 不使用)。
- 実機ランタイムは WAMR。まず interpreter で動かし、AOT は後続フェーズ。

---

# Phase 0 調査結果 (2026-07-05)

## 1. 既存コンポーネント構成

ESP-IDF v5.5.1 / C++17 / target esp32s3。プロジェクトルートは `src/`。

| コンポーネント | 内容 |
|---|---|
| `main/app_main.cpp` | NVS→PowerKey→Display→Touch→Ui→Audio init→SD マウント(別タスク)→再生要求の配線→200ms ポーリングのメインループ |
| `components/display` | SPI(SPI2, 40MHz) + ST7789 240x320 + esp_lvgl_port。LVGL バッファ 240x40 ダブル (~38KB DMA) |
| `components/touch` | esp_lcd_touch(I2C 静電容量) |
| `components/ui` | LVGL 9.4。ファイル一覧+OK ボタン+ステータスラベル |
| `components/audio` | chmorgan/esp-audio-player (helix MP3) + I2S std mode (BCLK=48, WS=38, DOUT=47, PCM5101)。`Audio_Init`/`Play_Music` 等の C API |
| `components/storage` | SD カード (FATFS, `/sdcard`) |
| `components/power_key` | 電源キー長押し検出 |
| `components/board` | ピン定義 |

管理依存 (registry): lvgl 9.4.0, esp_lvgl_port 2.6.2, esp_lcd_touch 1.1.2,
esp-audio-player 1.0.7, esp-libhelix-mp3 1.0.3。

LVGL 描画は esp_lvgl_port のタスクが行い、他タスクからの UI 操作は
`lvgl_port_lock()/unlock()` または `lv_async_call()` 経由(既存コードの慣例)。

### 現状の sdkconfig で注意すべき点

- **PSRAM 無効** (`CONFIG_SPIRAM` not set)。ボード実装は 8MB PSRAM 搭載
  ([Waveshare 仕様](https://www.waveshare.com/esp32-s3-touch-lcd-2.8.htm):
  16MB Flash + 8MB PSRAM, 512KB SRAM)。
- **フラッシュ 2MB 設定・single app パーティション**(factory 1MB)。実チップは 16MB。
  現状の app バイナリは約 741KB → WAMR 追加で 1MB を超える可能性あり。超えたら
  custom `partitions.csv` で app 領域を拡大する(実チップに余裕あり)。
- CPU 160MHz(240MHz まで可)、最適化 -Og。計測時は条件として記録する。

## 2. WAMR 統合方式の比較

| 方式 | 評価 |
|---|---|
| **(A) ESP Component Registry `espressif/wasm-micro-runtime`** | **採用。** 最新 2.4.0~1 (2025-08)。IDF ≥5.1、esp32s3 対応を確認済み。既存の managed_components と同じフローで、`idf.py` の configure 時に取得され `dependencies.lock` で固定される → **Docker ビルドコンテナ内で完結**(既に registry 依存を使っており CI も同じ経路)。menuconfig「Component config → WASM Micro Runtime」で interpreter 種別等を設定可能 |
| (B) wasm-micro-runtime リポジトリを component として取り込む (submodule/vendor) | 制御性は最高だがメンテコストが高い。(A) で不足(パッチ必要等)が出た場合のフォールバック |
| (C) espressif/esp-wasmachine | WASI・シェル・App Manager・LVGL バインディングまで持つフル実装で PoC には過大。**参考実装として読む**(native symbol 登録、メモリ設定、LVGL 連携の実例) |

Rust 側ツールチェーン: ホストに rustup + rustc 1.95.0 あり。
`rustup target add wasm32-unknown-unknown` の追加のみ必要。ESP-IDF ビルダーコンテナには
Rust は入っていないため、**.wasm のビルドはホスト側**(または将来 CI に rust ステップ追加)。
.wasm はバイナリ成果物としてファーム側ビルドに渡すので、コンテナ完結性は損なわない。

## 3. メモリ見積り(interpreter + 小さな .wasm)

内部 SRAM 512KB のうち、現状アプリ(LVGL バッファ ~38KB 含む)使用後の空きヒープは
**未実測**(Phase 1 冒頭で `esp_get_free_heap_size()` をログして確認する)。

WAMR 側の想定(要実測、一般的な実績値ベース):

- コードサイズ: interpreter 構成で +100〜200KB flash 程度。
- ランタイム RAM: WAMR グローバルヒーププール(可変、まず 128KB で試す)
  + モジュールインスタンス(linear memory + exec stack)。
- .wasm 側: no_std Rust でスタックサイズを縮小(`-C link-arg=-zstack-size=8192` 等)
  すれば linear memory は 1〜2 page (64〜128KB)。デフォルトのままだと Rust は
  スタック 1MB を要求するので必ず縮小する。
- 合計 ~150〜250KB を内部 SRAM から確保する見込み。**成立見込みだが実測が前提。**

フォールバック: PSRAM 有効化(8MB)し WAMR ヒープを PSRAM に置く。ただしアクセス
レイテンシが増えジッタ計測条件が変わるため、まず内部 SRAM で試す。

no_std Rust の現実性: **現実的**。カウンタ+クリック音のデモはアロケータ不要で、
`#![no_std]` + `panic_handler` + `crate-type = ["cdylib"]` + import/export の
`extern "C"` のみで書ける。バイナリは数 KB(LTO + opt-level="z" + strip)。

## 4. ホスト API 案(v0)

WASM 側 import(module 名 `env`、WAMR の native symbol として登録):

```c
// 文字列は (ptr, len) で linear memory 内を指す。ホスト側で addr 変換・境界検証。
void     hostapi_draw_text(int32_t x, int32_t y, uint32_t str_ptr, uint32_t str_len);
void     hostapi_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb888);
void     hostapi_play_click(void);
uint32_t hostapi_now_ms(void);   // 起動からの経過 ms(32bit で PoC には十分)
```

- 色は `0xRRGGBB`(プラットフォーム非依存。実機側は `lv_color_hex()` に直結)。
- WASM 側 export(ホストが呼ぶ):`app_init()`, `app_tick()`。
  tick 駆動はホスト側タスク(周期可変)が行う。タイミングの主導権をネイティブに置く
  方針と一致し、Phase 4 のジッタ計測(呼び出し間隔の実測)もこの層で行える。
- 実機側の描画実装: wasm デモ用スクリーン上の LVGL オブジェクト(fill_rect は
  lv_obj または lv_canvas、draw_text は lv_label)を `lvgl_port_lock()` 下で操作。
- click 音: audio コンポーネントに `play_click()` を追加(短い生成 PCM を I2S へ書く。
  wasm デモモードでは MP3 プレイヤーは起動しないので I2S の競合なし)。

## 5. 起動モード分岐案

Kconfig `CONFIG_MIDIBOX_WASM_DEMO`(ビルド時分岐)を採用。
`app_main` の冒頭で分岐し、有効時は wasm デモ画面+ランタイム起動、無効時は従来の
MP3 デモをそのまま実行。既存機能に対する変更が最小で、壊すリスクがない。
(実行時分岐(SD に app.wasm があれば wasm モード等)は PoC 後に検討。)

## 6. .wasm のロード方法

Phase 1〜2 はフラッシュ埋め込み(`idf_component_register(... EMBED_FILES app.wasm)`)を
採用。SD 不要・確実で、ビルドフローも単純。SD ロードは反復開発が辛くなったら追加検討。

---

# Phase 1 以降の詳細計画

## Phase 1: WAMR がただ動く

1. `idf.py add-dependency "espressif/wasm-micro-runtime"`(バージョン固定)。
2. `src/components/wasm_runtime` を新設:
   - `WasmRuntime` クラス: `wasm_runtime_full_init`(Alloc_With_Pool, まず 128KB 静的プール)
     → load → instantiate → `app_init` 呼び出し → デイニット。
   - 最小 .wasm(整数を返すだけ)。Rust no_std で `wasm-apps/hello/` に作成し、
     ビルド済み .wasm をリポジトリにコミット(再現手順は README 化)。EMBED_FILES で埋め込み。
3. `app_main` に Kconfig 分岐を追加し、wasm モードで実行結果と
   前後の free heap をログ出力。
4. 完了条件: 実機ログに .wasm の返り値と heap 実測値が出る。MP3 モードのビルド・動作が
   従来どおりであること。

計測メモ(この時点で記録): WAMR 追加によるバイナリサイズ増分、init/instantiate の
所要時間、free heap 前後差。app パーティション 1MB 超過ならここで partitions.csv 対応。

### Phase 1 実施記録 (2026-07-05) — 完了

実機ログで selftest PASS(`app_init()` が 42 を返却)。MP3 モード(=n)も
ビルド・フラッシュして UI ready / SD マウントまで回帰確認済み。

構成: WAMR 2.4.0~1 (registry) / fast interpreter。WAMR の Kconfig 既定から
AOT・LIB_PTHREAD・LIBC_WASI・APP_FRAMEWORK を無効化(interp + libc builtin のみ)。

実測値(-Og, 160MHz, fast-interp):

| 項目 | 値 |
|---|---|
| wasm モード起動直後の free heap(LVGL/audio 未初期化) | 226,364 bytes |
| WAMR full_init→load→instantiate→call 合計 | 26.1 ms |
| モジュールロード中の malloc ヒープ消費(静的プール外) | ~17.8 KB |
| WAMR 静的ヒーププール(BSS 確保) | 128 KB |
| WAMR コード flash 増分(libespressif__wasm-micro-runtime.a) | 66.4 KB .text + ~2 KB DIRAM |
| バイナリ: wasm モード / MP3 モード | 338 KB(LVGL 等が GC される)/ 769 KB(基準 741KB +28KB) |
| hello.wasm(Rust no_std, opt-z, lto, strip, -zstack-size=8192) | 106 bytes |

いずれも 1MB app パーティション内 → **partitions.csv 対応は不要**(Phase 2 以降で再確認)。

ハマりどころ(Phase 2 以降も前提となる知見):

1. **WAMR component の Kconfig 既定は全部盛り**(LIB_PTHREAD/WASI/APP_FRAMEWORK/AOT
   有効)。LIB_PTHREAD(=THREAD_MGR)が有効のままだと `wasm_runtime_create_exec_env`
   が失敗した。sdkconfig で無効化して解決。
2. **interpreter モードの `wasm_runtime_load` は渡したバッファを module 生存中
   参照し続ける**(fast-interp は in-place 書き換えもする)。load 直後に free すると
   export 名文字列が化けて lookup が失敗する。可変コピーを unload まで保持すること。
3. **WASM を実行するスレッドは pthread で作る**(`esp_pthread_set_cfg` +
   `pthread_create`)。WAMR esp-idf 層の `os_self_thread()` が `pthread_self()` を
   呼ぶため、素の `xTaskCreate` タスクから呼ぶと
   `assert failed: pthread_self` でパニックする。

現状: sdkconfig は `CONFIG_MIDIBOX_WASM_DEMO=y`(PoC 進行中のデフォルト)。
実機には回帰確認時の MP3 モードビルドが書き込まれたまま。

## Phase 2: ホスト API とデモアプリ

1. §4 のホスト API 4 本を native symbol 登録(`wasm_runtime_register_natives`)。
   文字列引数は `wasm_runtime_validate_app_addr`+`addr_app_to_native` で検証。
2. audio に `play_click()` 追加(生成 PCM、例: 1kHz 減衰サイン 30ms)。
3. `wasm-apps/demo/`: 1 秒ごとにカウンタを描画しクリック音を鳴らす Rust アプリ。
   ホスト側 tick タスク(周期 100ms 程度、アプリ側で 1s を判定)から `app_tick()` を呼ぶ。
4. 完了条件: 実機の画面にカウンタが 1 秒ごとに更新され、クリック音が鳴る。

### Phase 2 実施記録 (2026-07-05) — 実装完了・実機ログ確認済み

実機で demo.wasm の tick ループが起動し、60 秒以上 trap /エラーなしで動作
(画面のカウンタ更新とクリック音はユーザーの目視/聴覚確認による)。

実装の要点:

- ホスト API 4 本を `wasm_runtime_register_natives("env", ...)` で登録
  (`components/wasm_runtime/hostapi.cpp`)。文字列は WAMR シグネチャ `"(ii*~)"` の
  `*~`(ptr+len)を使い、**WAMR 側で境界検証済みのネイティブポインタを受け取る**
  (手動の validate_app_addr は不要)。
- 描画モデル v0: (x,y) をキーにした retained LVGL オブジェクト
  (text=lv_label / rect=lv_obj)。同一座標への再描画は既存オブジェクトの更新。
  スロット固定 16+16、あふれは警告ログ。lvgl_port_lock 下で操作。
- クリック音: audio に `Audio_Click_Init()`(I2S のみ初期化、esp-audio-player の
  タスクは起こさない)と `Play_Click()`(1kHz 減衰サイン 30ms 生成 PCM、初回生成を
  static バッファにキャッシュ)を追加。MP3 経路とは排他(wasm モードでは
  audio_player 不使用)。
- demo 実行: `wasmrt::run_demo()` が pthread("wasm_demo", stack 16KB)を起こし、
  app_init() 後に 100ms 周期で app_tick() を呼ぶ(1 秒判定はアプリ側)。
- `wasm-apps/demo/`: no_std Rust、730 bytes。static mut でカウンタ状態を保持
  (app_init/app_tick は同一スレッド前提という契約)。

実測値(-Og, 160MHz, fast-interp):

| 項目 | 値 |
|---|---|
| バイナリ(wasm モード, LVGL+WAMR 込み) | 710 KB(1MB パーティション内 32% 空き) |
| demo モジュールロードの malloc 消費 | ~18.5 KB |
| 全初期化後(Display+LVGL+I2S+WAMR+module)の free heap | **56 KB** |
| demo.wasm | 730 bytes |

メモ: free heap 56KB はまだ回るが余裕は薄い。WAMR プール 128KB が最大の固定費。
Phase 4 で `wasm_runtime_dump_mem_consumption` を見てプール縮小を検討する。

## Phase 3: Linux ホスト

- `hosts/linux/` に C で最小ホストを作成。**ランタイムは Linux 側も WAMR を推奨**:
  - 理由: native symbol 登録テーブルとホスト API シグネチャを実機側と共通の
    ヘッダ/ソースで共有でき、「同一 .wasm・同一 API」の検証として最も直接的。
    WAMR は CMake で vmlib として容易にビルド可能(FetchContent または submodule)。
  - 対案 wasmtime: ツールリング・デバッグ性は上だが、ホスト関数登録の書き方が
    別物になり共有できない。ランタイム差異の検証価値は PoC の完了条件に含まれない。
- 描画: SDL2(要 apt)+ 組込み 8x8 ビットマップフォント(font8x8, public domain ヘッダ)。
  音: SDL audio に生成 PCM。時刻: `clock_gettime(CLOCK_MONOTONIC)`。
- 完了条件: Phase 2 と同一の .wasm バイナリが Linux 上で同じ動作をする。

## Phase 4: 計測と記録 → `docs/poc-results.md`

計測は実機・release 相当条件を明記(CPU 周波数、最適化レベル、interpreter 種別)。

1. ホスト API 呼び出しコスト: wasm 内ループから `hostapi_now_ms` を N 回呼び、
   `esp_cpu_get_cycle_count()` でサイクル数/回を算出。ネイティブ直呼びと比較。
2. ジッタ: tick タスクの起床間隔と `app_tick()` 実行時間を `esp_timer_get_time()` で
   数千サンプル収集し、min/avg/max/分布を記録。
3. メモリ: flash 増分、WAMR プール消費、instantiate 後 free heap、
   `wasm_runtime_dump_mem_consumption` の出力。
4. 所見: interpreter のままで音楽アプリロジックに足りるか、AOT へ進む判断材料を書く。

## 未確定・要実測事項(推測で進めない)

- 初期化後の内部 SRAM 空きヒープ実測値(Phase 1 で取得)。
- WAMR 2.4.0 component の実フットプリント(flash/RAM)と menuconfig 推奨値。
- fast interpreter(既定, メモリ2倍・速い)と classic interpreter の選択 → Phase 1 は
  既定で開始し、メモリが厳しければ classic を試す。
- I2S クリック音の初回発音レイテンシ(Phase 2 で確認)。

---

# 依存の記録

| 依存 | 追加フェーズ | 理由 |
|---|---|---|
| `espressif/wasm-micro-runtime` (registry, 2.4.0 系固定) | Phase 1 | WASM ランタイム本体。registry 経由が既存ビルドフロー(managed_components + Docker + CI)と整合し追加コスト最小 |
| (Linux) WAMR vmlib, SDL2 | Phase 3 | Linux ホスト用。実機と同一ランタイムで API 登録コードを共有するため |

# ビルドメモ

- Docker: `ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5`(README 参照)。tmux pane 1 で実行。
- `src/` で `idf.py build` / `idf.py flash`。
- CI: `.github/workflows/build.yml` が devcontainer で `idf.py build`。
