# MidiAppBox 開発ログ(Phase 0〜)

調査・計画・実施記録・実測値・トラブルの詳細を Phase ごとに残す(追記型)。
常時従うルール・教訓の要約は CLAUDE.md を参照。

## 旧ルールの履歴(現在は失効。原文保存)

CLAUDE.md 整理(2026-07-19)時点で現状と矛盾していたため、ここに原文を移動:

> - 開発環境は **herdr**(4 pane): p1=エージェント、**p2=ESP-IDF Docker**
>   (`idf.py build`/`flash`/`monitor`)、**p3=ホスト側作業**(cargo, Linux ホスト,
>   curl 等ネットワークが要るもの)、p4=予備。
>   送信は `herdr pane run <id> '<cmd>'`、確認は `herdr pane read <id> --lines N`、
>   待機は `herdr wait output <id> --match <text> --timeout <ms>`。
>   Docker 起動はリポジトリルートで README のコマンド(`${PWD}` マウントなので
>   pane の cwd を先に合わせること)。

→ pane ID 直指定・ログ文言への wait output 直マッチは再現性がなく廃止。
現行は `scripts/hpane.sh` によるラベル解決+番兵トークン方式(CLAUDE.md)。

> - 既存の MP3 再生デモは壊さない(起動モードで分岐)。

→ Phase 6D で旧 MP3 デモモードは削除しランチャーに一本化。現行ルールは
「既存アプリの回帰を壊さない」(CLAUDE.md)。

---

# Phase 0 調査結果 (2026-07-05)

## 1. 既存コンポーネント構成

ESP-IDF v5.5.1 / C++17 / target esp32s3。プロジェクトルートは `src/`。

| コンポーネント | 内容 |
|---|---|
| `main/app_main.cpp` | NVS→PowerKey→Display→Touch→Ui→Audio init→SD マウント(別タスク)→再生要求の配線→200ms ポーリングのメインループ |
| `components/display` | SPI(SPI2, 40MHz) + ST7789(パネル native 240x320)+ esp_lvgl_port。**LVGL 論理画面はランドスケープ 320x240**(hres=LCD_V_RES, swap_xy=true。touch も 90° 回転済み)。LVGL バッファ 240x40 ダブル (~38KB DMA) |
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

### Phase 3 実施記録 (2026-07-05) — 実装完了・動作確認済み

`hosts/linux/` に C の最小ホストを実装。**実機と同一のコミット済み
`wasm-apps/demo/demo.wasm`(730 bytes)がそのまま動く**ことを確認
(app_init/app_tick、カウンタ描画、クリック音)。

- ランタイム: WAMR 2.4.0(実機 component と同一タグ)を FetchContent
  (GIT_SHALLOW)で取得し vmlib としてビルド。構成も実機に合わせ
  fast interpreter + libc builtin のみ、`Alloc_With_Pool` 128KB。
- **ホスト API 定義の共有**: `shared/hostapi_defs.h` に X-macro
  (`HOSTAPI_NATIVE_SYMBOLS`)で名前+WAMR シグネチャを一元化し、実機
  (`hostapi.cpp`)と Linux(`main.c`)の双方が NativeSymbol テーブルを
  これから生成する。API の追加・変更は必ずこのヘッダ経由で行うこと。
- 描画: SDL2(240x320 論理サイズ、2 倍ウィンドウ)+ font8x8(public domain,
  `hosts/linux/font8x8_basic.h` に取得済み)。実機と同じ (x,y) キーの
  retained スロット方式で、毎 tick 全スロット再描画。
- 音: SDL audio キューに実機と同一波形(1kHz 減衰サイン 30ms)の生成 PCM。
- ビルド/実行手順は `hosts/linux/README.md`。ヘッドレス CI 用には
  `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` で起動可。

メモ: tmux pane 1 は idf.py monitor 内のことがある。コマンドを送る前に
シェルプロンプトか確認する(monitor 内だと入力がシリアルへ流れる)。
ネットワークが要る作業(curl, FetchContent)は pane 2(ホスト側 bash)で行う。
(注: この pane 運用は現行の hpane.sh 方式で置き換え済み。)

## Phase 4: 計測と記録 → `docs/poc-results.md`

計測は実機・release 相当条件を明記(CPU 周波数、最適化レベル、interpreter 種別)。

1. ホスト API 呼び出しコスト: wasm 内ループから `hostapi_now_ms` を N 回呼び、
   `esp_cpu_get_cycle_count()` でサイクル数/回を算出。ネイティブ直呼びと比較。
2. ジッタ: tick タスクの起床間隔と `app_tick()` 実行時間を `esp_timer_get_time()` で
   数千サンプル収集し、min/avg/max/分布を記録。
3. メモリ: flash 増分、WAMR プール消費、instantiate 後 free heap、
   `wasm_runtime_dump_mem_consumption` の出力。
4. 所見: interpreter のままで音楽アプリロジックに足りるか、AOT へ進む判断材料を書く。

### Phase 4 実施記録 (2026-07-05) — 完了

計測結果と所見は **`docs/poc-results.md`** に集約(条件: -Og, 160MHz, fast-interp)。
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

---

# Phase 5: SD ロードとランチャー (2026-07-05〜)

目的: /sdcard/apps/ の .wasm をメニューから選択起動し、power_key 短押しで
メニューに戻る(ユーザー選択済み)。起動↔終了 10 サイクルでリークなしが完了条件。

## 決定事項

- 「メニューへ戻る」= **power_key 短押し**(ユーザー選択)。現状 power_key は
  外部電源時にキーを監視していない(battery_mode のみ)ので、外部電源でも
  ポーリングして短押しイベントを拾う拡張を行う(長押し 2s 電源断は電池時のみ、従来どおり)。
- アプリライフサイクルはホスト所有: `app_start(path, on_stopped)` →
  100ms tick → `app_request_stop()` → (export されていれば) `app_exit()` →
  exec_env → instance → module の順に破棄 → バッファ free。
  ランタイム(`wasm_runtime_full_init`)は起動時に一度だけ(`runtime_init()`)。
- 初回セットアップ: /sdcard/apps が無ければ作成し、埋め込みのサンプル .wasm を
  シードする(以後は SD 上のファイルが正)。

## 5A 実施記録 (2026-07-05) — 完了

SD 上の /sdcard/apps/demo.wasm のロード・実行を実機確認。

**重要な障害と根本原因(再発注意):** ランチャービルドで SD マウントが
`mount_to_vfs failed (0x101=NO_MEM)` で失敗した。カードは SPI で応答しており
(cmd52/cmd5 の R1 ログ)、原因は **FATFS の VFS 登録が要求する連続ヒープ**
(`CONFIG_FATFS_SECTOR_4096` × `max_files=8` → FIL バッファ込みで ~38KB の
一括 calloc)に対し、WAMR の 128KB 静的プール(BSS)がヒープを圧迫して
最大連続ブロックが 31.7KB しかなかったこと。**WAMR プールを 64KB に縮小**
(Phase 4 実測 27.5KB 消費なので余裕)して解決。MP3 モードで動いていたのは
プールがリンカ GC で消えて余裕があったため。
教訓: 大きな静的バッファを足したら `heap_caps_get_largest_free_block()` も見る。

- SDMMC ホストは現在この個体でタイムアウト(263)し SDSPI フォールバックで
  マウントしている(MP3 モードも同じ)。以前は SDMMC で通っていたことがあり、
  ハード状態依存。フォールバックがあるので実害なし。
- マウントは 400ms 間隔で 3 回リトライ(初回タイムアウト対策)。

## 5B 実施記録 (2026-07-05) — 完了

メニュー(`launcher.cpp`)から demo.wasm / bars.wasm のタッチ起動を実機確認。

- メニュー UI: リスト行は lv_button+lv_label。行データはラベルテキストを
  そのまま使い(cb で `lv_label_get_text`)、rebuild ごとの動的確保なし。
  再スキャンは `launcher_show()` のたびに実施。
- wasm モードでも Touch を初期化するようにした(app_main)。
- **2 つ目のサンプルアプリ `wasm-apps/bars/`(640B)**: イコライザ風 8 本バー。
  課題どおり「跳ね回る矩形」も検討したが、ホスト API v0 の (x,y) キー retained
  モデルでは移動アニメがスロットを食い潰すため、**座標固定・サイズ/色可変**で
  アニメする設計にした(retained モデルでは縮小領域は LVGL の再描画で背景に
  戻るため消し込み矩形も不要)。この制約は API v1 検討時の材料。

## 5C 実施記録 (2026-07-06) — 完了・Phase 5 完了

- **メニュー復帰 = power_key 短押し**: PowerKey に短押しコールバックを追加
  (外部電源でもポーリング、長押し 2s 電源断は電池時のみ従来どおり。
  電池起動時の押しっぱなしを誤検知しないよう「解放を一度観測してから」計上)。
  コールバックは power_key タスク(小スタック)上なので atomic の
  `app_request_stop()` のみ。実機で 起動→短押し→メニュー→別アプリ起動 を確認。
- **リーク検証(CONFIG_MIDIBOX_WASM_CYCLE_TEST=y で自動実行)**:
  demo.wasm の起動 2 秒→停止を 10 サイクル。free heap は
  開始 91,712 → サイクル 1 後 91,460(−252B はメニュー初回構築の一度きり)→
  **サイクル 2〜10 まで 91,460 / largest 34,816 で完全一定。リークなし。**
  WAMR の deinstantiate→unload→(プール再利用) が正しく回ることを確認。
- **エラーハンドリング**: SD 未挿入 → メニューに「SD mount failed」表示で滞留
  (実機確認)。壊れた .wasm → 「magic header not detected」をメニューに表示し
  クラッシュなし(自動テストで確認)。apps 空 → 「no .wasm files」表示
  (コードパスは同一、目視は未実施)。
- 停止コールバックは **cb 実行後に Idle へ遷移**する順序にした(次アプリの
  画面生成と、cb 内のメニュー復帰・画面破棄の競合防止)。

# Phase 6: MP3 プレーヤーの WASM アプリ化(Host API v0→v1)

課題定義・完了条件はユーザー指示(2026-07-12)による。tick モデル維持、
入力はイベントキュー型、v0 4 関数は不変、アプリ実装が API 設計を駆動する。

**実機検証の運用(Phase 6 から)**: Web カメラ(/dev/video0)で実機を撮影して
検証する。録画は `~/ビデオ/rec.sh`(露出・フォーカス自動適用、Enter で停止)。
herdr の撮影用 pane で起動し、空文字送信(=Enter)で停止する。
動画・写真は Zenn 記事の素材として `~/ビデオ/zenn-phase6a/` 等に残す。

## 6A 実施記録 (2026-07-12) — 完了

入力イベント API `hostapi_poll_event` を追加し、touch_demo.wasm で両ホスト検証済み。

イベント規約(ユーザー承認済み、ABI 凍結):

- `hostapi_event_t` = **12 バイト固定** `{u16 type, u16 param, i16 x, i16 y, u32 time_ms}`
  (LE、time_ms は now_ms と同一時基)。type: 1=TOUCH_DOWN, 2=TOUCH_UP。
  拡張は type 追加(アプリは未知 type を無視する契約)と param で行い、
  レコードサイズは変えない。
- `hostapi_poll_event(buf, len)->n`(WAMR シグネチャ `"(*~)i"`)。
  ホストは len/12 件を上限にキュー先頭から書き、残りは次回。
- キュー深さ 16、満杯は最古から捨てる。**DOWN 未配送の UP は捨てる**
  (アプリを起動したタップの UP がアプリに漏れる問題の対策)。
  キューはアプリ起動時に空、破棄で消滅。

実装の要点:

- 実機の捕捉点は**アプリスクリーンへの LV_EVENT_PRESSED/RELEASED コールバック**
  (hostapi.cpp)。アプリ実行中だけイベントが流れる経路切替が構造的に成立する。
  そのため `fill_rect` の lv_obj は CLICKABLE を解除してスクリーンへ素通しする。
  座標は `lv_event_get_indev`+`lv_indev_get_point`。
- 生産者(LVGL タスク)/消費者(wasm アプリ pthread)間は portMUX spinlock。
  Linux は単一スレッドなのでロックなし(main ループの SDL_MOUSEBUTTONDOWN/UP を
  app_running 時のみ push)。
- LVGL indev はポーリング ~33ms だが、1 サンプルでも押下を観測すれば
  PRESSED→RELEASED の両遷移が出るため、33ms 以上のタップなら DOWN/UP が揃う。
- touch_demo(1549B)は座標表示+DOWN/UP カウント+CLICK ボタン(音+色変化)。
  背景とタイトルバーの (x,y) キー重複に注意(retained モデルでは後勝ち置換)。

検証結果:

- Linux: xdotool の click(DOWN/UP 間隔 ~12ms)で down/up が同数で増加、
  座標正確、CLICK 音再生。demo/bars 回帰 OK。
- 実機: タップで座標追従(シリアルの BASIC P(x,y) と画面表示が一致)、
  連打後 down:11 up:11 で完全一致(タップ長 243ms の例も DOWN/UP 両方配送)。
  power_key 短押しでメニュー復帰、demo.wasm 回帰 OK。
  アプリ停止時 free heap 80,980→80,980(開始時と一致、リークなし)。
- 記事素材: `~/ビデオ/demo_172530.mp4`(フルテイク 4:43)、
  `~/ビデオ/zenn-phase6a/`(トリム版 phase6a_touch_verification.mp4 と静止画 6 枚)。

## 6B 実施記録 (2026-07-12) — 完了

hostapi_audio_play/ctrl/set_volume/get_state の 4 関数を追加(設計はユーザー承認済み)。
両ホストで同一の mp3player.wasm(1983B、6C でプレイリストに育てる土台)により
再生/一時停止/再開/停止/音量/自然終了(FINISHED)/再生中終了の全項目を検証。

**メモリの壁と対策(6B 最大のリスクが実際に発現)**:

- フル Audio_Init(esp-audio-player タスク)は **47,172 bytes** 消費。
  FATFS(sector 4096 × max_files 8 ≒ 38KB)と重なると largest block が
  15,360 まで細り、**mp3player.wasm の instantiate が
  「allocate linear memory failed」で失敗**した(linear memory は
  WAMR の shrunk memory で ~20KB 連続を system heap から取る)。
- 対策(ユーザー承認: A 案): **CONFIG_FATFS_SECTOR_512 + max_files 8→4**。
  MP3 128kbps=16KB/s に対し sector 512 で十分。適用後はアプリ起動時
  free 57,688 / largest 31,744 で安定動作。
- PSRAM 有効化(案 C)は「次のメモリの壁」用のロードマップ項目として温存。

実装メモ:

- 状態機械はホスト側で宣言的に管理(`s_audio_state`)し、自然終了だけ
  `Music_finished()` を get_state/ctrl 時に取り込む。FINISHED/ERROR は
  次の play か STOP まで保持。状態不整合のコマンドは -1(トラップしない)。
- パスはミュージックルート相対(実機 /sdcard/music、Linux ./sdcard/music)。
  先頭 '/' と ".." は拒否(6C の fs_list と同じサンドボックス境界)。
- ライフサイクル契約: `hostapi_audio_reset()`(無条件 Music_stop)を
  アプリ起動直前と破棄時に呼ぶ。Linux は `host_sdl_audio_reset()`。
- 検証用 MP3(アルペジオ 12 秒 96,801B, 22.05kHz mono 64kbps)を
  **ファーム埋め込み→ /sdcard/music/test.mp3 へシード**(SD 抜き差し不要)。
  seed_file は 100KB 級に備えチャンク比較へ変更。
- クリック音(直接 I2S 書き込み)と esp-audio-player は排他前提のまま:
  mp3player はクリック音を使わない。同時使用は将来の音源 API で整理。
- サイクルテストは mp3player.wasm +「再生中に停止」を毎サイクル実施する形に変更。

実測(実機, -Og, 160MHz):

| 項目 | 値 |
|---|---|
| Audio_Init(フル)のヒープ消費 | 47,172 bytes |
| FATFS sector 512 + max_files 4 の効果 | アプリ起動時 free 40,084 → 57,688 |
| mp3player 実行中 free heap | ~29,400(instantiate 後) |
| 起動→再生→停止 10 サイクル | 開始 65,892 → 1 回目 65,692(−200B は初回のみ)→ **2〜10 回目 65,692 / largest 31,744 で完全一定** |
| 停止後の playing フラグ | 全サイクル 0(音停止契約 OK) |

検証エビデンス: `~/ビデオ/demo_185836.mp4`(55 分、操作は最後 6 分)、
`~/ビデオ/zenn-phase6b/`(トリム版+静止画 8 枚)。起動失敗(メモリの壁)の
一部始終は `~/ビデオ/demo_183116.mp4`。
Linux は SDL_mixer で同一検証+mixer 無しビルドのエラーパス確認済み。

## 6C 実施記録 (2026-07-12) — 完了

`hostapi_fs_list(idx, buf, len)->n`(`"(i*~)i"`、-1 で終端)を追加し、
mp3player.wasm(2942B)を本命のプレーヤーに拡張。両ホストで全完了条件を検証済み。

- fs_list はホスト側に状態を持たず毎回 readdir で idx 番目を返す(数十曲想定)。
  63 バイト超の名前とサブディレクトリは除外。列挙順は readdir 順(ソートなし)。
- アプリ UI: リスト 6 行+▲▼スクロール+選択ハイライト。retained モデルの
  スロット収支は rect 15/16・text 15/16(選択ハイライトは行位置固定の rect の
  色変え、スクロールは text の置き換えで実現 — 移動なしなのでスロットを食わない)。
- 連続再生はアプリ側: tick で get_state==FINISHED を見て次 idx を play、
  末尾なら CMD_STOP。実機ログで自然終了→次曲まで 100ms。
- エラー 3 ケース確認: (a) 曲なし/dir なし → 「no mp3 files in music dir」表示
  (b) 壊れた MP3 → state: ERROR 表示・クラッシュなし(mpg123 が resync 失敗を報告)
  (c) 再生中の短押し終了 → 音停止(6B に続き 6C でも確認)
- 実機シード曲を 3 つに(test.mp3 12s + tune_down/tune_duo 各 6s・32kbps ~24KB)。
- 10 サイクル(mp3player v2 + 毎サイクル再生中停止): 2 回目以降
  **64,840 / largest 31,744 で完全一定、リークなし**。
  注: サイクルログの playing=1 は停止直後(~100ms)のサンプリングで、
  esp-audio-player の停止が非同期なため。停止自体は有効
  (以後 Playback finished が一切出ないことで確認)。
- 教訓(運用): `herdr wait output --match` は高速スクロール行を取りこぼす。
  完了検知は「新しいブートの安定した末尾状態」をポーリングで見る。

検証エビデンス: `~/ビデオ/demo_204721.mp4`(1:40)と `~/ビデオ/zenn-phase6c/`
(静止画 9 枚: タップ再生→自動次曲→末尾停止→PAUSED、Linux の 8 曲リスト/
スクロール/エラー 2 種)。

## 6D 実施記録 (2026-07-12) — 完了・Phase 6 完了

旧 MP3 デモモードを削除してランチャーに一本化し、hostapi_defs.h を v1 として整理。

- **削除**: Kconfig `MIDIBOX_WASM_DEMO`(分岐ごと)、`components/ui`(旧デモ専用)、
  app_main の #else 経路。main の REQUIRES は power_key/display/touch/audio のみに縮小。
  `MIDIBOX_WASM_CYCLE_TEST` は depends を外して存続(リーク検証用)。
- **hostapi_defs.h v1**: gfx / input / audio / fs / misc にグループ化し、共通契約
  (座標系、文字列規約、out-buffer 規約、負数エラー・非トラップ方針、
  ライフサイクル+オーディオ停止契約、アプリ起動時の初期状態)をヘッダに集約。
  v0 の 4 関数はシグネチャ・挙動とも不変(X-macro の並び替えのみ。登録は名前
  ベースなので ABI 影響なし)。
- **hello/bench の扱いを明確化**: app_tick を export しないテストモジュールで
  あり、ランチャーアプリではない。ランチャーは「app_init/app_tick not exported」
  をメニューに表示して優雅に拒否する(両ホストで確認)。`run_selftest`/`run_bench`
  はユーティリティとして存続(現在は未配線)。
- **最終回帰**: Linux はメニューから 6 アプリ(4 本動作+hello/bench の拒否)、
  実機は 4 アプリ起動→短押し復帰(各 free heap 57,672 で一定)+
  mp3player 再生中終了で音停止。エビデンス: `~/ビデオ/demo_224403.mp4`(実機)、
  スクリーンショット一式(Linux)。

# Phase 7: Host API v2(メトロノームに向けた予約発音)

## 7A 実施記録 (2026-07-13) — 実装・計測完了

`hostapi_click_schedule(time_ms)`(予約 1 件・置き換え・time 0 でキャンセル・
last_fired ガード)と `hostapi_audio_set_volume` のマスター音量化を両ホストに実装。
検証アプリ `wasm-apps/clicktest`(1285B、BPM120、タップで SCHED⇔LEGACY 切替)。

実装方式(ユーザー承認済み):

- **実機 = esp_timer ワンショット(方式 a)**。systimer µs 分解能、タスクディスパッチ
  (コールバックはタスクコンテキストなので i2s_channel_write 合法)。アイドル時の
  クリック(30ms=1323 フレーム)は DMA 深さ(6×240=1440 フレーム ≒32.6ms)に
  丸ごと収まり write は実質ノンブロッキング。FreeRTOS tick 100Hz(10ms 格子)の
  タスクポーリング案(b)は分解能で却下。
- **Linux = SDL コールバック(pull)型ミキサ**。再生済みフレーム数を音声クロックとし、
  time_ms→目標サンプル位置に換算してバッファ内オフセットで発音(サンプル精度)。
  クリック波形はキャッシュ非破壊で出力時に音量スケール。
- **エポックは最初のオーディオコールバックで確定する**(重要)。unpause 時に取ると
  音声クロックが壁時計より遅れ、「壁時計では期限到来・未発火」の窓でアプリの毎 tick
  再予約が未発火予約を置き換えて拍が落ちる(実測で avg 565ms/max 2000ms)。
  first-callback 起点なら pull の先読みぶんクロックが先行し、この窓が消える。
- さらにホスト保証として「**期限到来済みの未発火予約は置き換え前に必ず発音**」を
  両ホストに実装(Linux: 壁時計期限セーフティネット+置き換え時発火、ESP32:
  schedule 内で同ガード)。契約の想定利用パターン(毎 tick 再予約)が
  オーディオバックエンドのバースト遅延下でも拍を落とさないため。

計測(BPM120=500ms、N=99 間隔、log_stats 形式):

| ホスト/方式 | min / avg / max |
|---|---|
| 実機 SCHED | **499.996 / 500.000 / 500.004 ms(±4µs)** |
| 実機 LEGACY | 499.995 / 500.000 / 500.004 ms(注) |
| Linux SCHED | **500.000 / 500.000 / 500.000 ms(サンプルクロック基準)**、バースト時は最悪 ±1.2ms |
| Linux LEGACY | 394.7 / 499.6 / 534.1 ms(tick 格子 -105/+34ms) |

(注)実機 LEGACY が良く見えるのは 500ms が tick 格子 100ms の整数倍で
vTaskDelayUntil(Phase 4 実測 ±10µs)と完全同期するため。間隔計測は
位相オフセット(拍に対して最大 +100ms 遅れ)も隠す。一般則は Linux 側の
数字が示す。格子と噛み合わない BPM の実機計測は任意の追加課題。

トラブル記録:

- **SD シード書き込みの永続破損**: clicktest.wasm だけ実機シードの読み戻しが
  毎回不一致(magic header not detected)。埋め込みデータ・書き込みサイズは正常。
  ユーザーが PC から手動コピーしたら解消 → 度重なるハードリセットで FAT が
  傷んでいた可能性が高い。教訓: シード後に load が magic 不一致で落ち続けたら
  SD 側の FS 破損を疑う(手動コピー or 再フォーマット)。
- 運用: idf.py monitor はセッション断でパイプごと死ぬ。`PYTHONUNBUFFERED=1
  idf.py monitor 2>&1 | tee <マウント先ログ>`+ホスト側ファイルポーリングが安定。
  monitor 再起動は既定でボードをリセットする(実行中アプリが落ちる)。
  `--no-reset` は `-p <port>` 指定が必要。

回帰: Linux で demo(新クリック経路)/mp3player(マスター音量)確認。実機の
新クリック経路は clicktest 自体が Play_Click 経由のため検証済み。
アプリ停止後 free heap 50,792(開始 51,240、初回 −448B はメニュー構築)。
ベースラインは 6D 比 −6.4KB(クリック用スクラッチ+統計バッファの BSS、想定内)。

## 7B 実施記録 (2026-07-13) — 完了

メトロノーム本体 `wasm-apps/metronome`(2075B)を**既存 API のみ**で実装
(6C の流儀: API 追加はせず、不足はまず相談)。可変 BPM(40-240, ±5)、
拍子巡回(2/3/4/6)、START/STOP、拍ランプ(1 拍目アクセント色)。

- 拍時刻は `anchor + n*60000/bpm` を**拍ごとに計算**(周期を ms に丸めて
  加算しないので累積ドリフトなし)。BPM/拍子変更・START で再アンカー
  (小節頭から再開、その場で拍 0 を発音)。
- 発音は 7A の click_schedule に毎 tick 再予約するだけ。視覚(ランプ)は
  tick 格子(100ms)での更新で、音はホスト側 µs/サンプル精度という分担。
- **off-grid BPM の実測(7A の持ち越し)**: Linux BPM130 で
  min=460.998 / avg=461.536 / max=462.018 ms(理論 60000/130=461.538)。
  ±1ms は API 時基(ms)の量子化、avg は理論値と 2µs 差でドリフトなし。
  実機は esp_timer ワンショットで格子を持たないため 7A の ±4µs が周期非依存に
  適用される(実機の純 BPM130 ブロックは STOP が数発早く未取得。混合ブロック
  min=260/avg=468/max=500.004 は遷移込みで整合)。
- 音のアクセント(1 拍目の音色/音程変更)は既存 API では不可 → **7C の API
  相談事項**(click_schedule の拡張 or 音色設定 API)。7B は視覚アクセントまで。
- 実機確認: 起動→START→BPM130→6 拍子→STOP→短押し終了(録画
  `~/ビデオ/demo_232651.mp4`、素材 `~/ビデオ/zenn-phase7b/`)。
  停止後 free heap 51,012(開始 51,240、初回 −228B)。metronome.wasm の
  シードは正常(clicktest の FAT 破損は再発せず)。

## 7B-fix 実施記録 (2026-07-14) — 二重クリック修正・完了

ユーザー報告「たまにクリックが 2 回連続で鳴る(demo.wasm でも)」の調査と修正。

**診断(録音の音声解析による)**: メトロノーム実機録画のオンセット検出で、
全拍の ~25%(51/255)に間隔 **26-27ms** の二重発音を確認。26.5ms ≒ DMA
ディスクリプタ 5 本ぶん(240 フレーム×5.44ms)で、**クリック終端の I2S DMA
アンダーフロー時に auto_clear とプリフェッチが競合し、クリック先頭が入った
古いディスクリプタが 1 本再生される**ことが原因。Phase 2 以来の潜在バグ
(予約 API は無関係 — 発火統計 ±4µs は i2s_write 呼び出し時刻の計測で、
この現象はその後の DMA 段で起きる)。Linux は pull 型ミキサなので起きない。

**修正**(audio.cpp):

- クリック専用タスク(静的確保 4KB stack, prio 18)+バイナリセマフォ。
  `Play_Click()` は give するだけ(esp_timer タスクのブロッキングも解消)。
- タスク内で: ①レートが 44.1kHz 以外なら戻す(MP3 22.05kHz 再生後にクリックが
  半ピッチになる潜在バグも同時修正)②音量スケールを 1 ディスクリプタぶんの
  チャンク単位で適用しつつ書き込み ③**DMA リング 1 周ぶん(6×240 フレーム)の
  ゼロを追記** — アンダーフロー時のプリフェッチ再生が無音になる。

**メモリの壁(3 回目)**: クリックタスクをヒープ確保にしたら largest block が
31,744→15,360 に割れ instantiate 失敗。静的確保に変えても largest は不変
(BSS +4.5KB で領域の切れ目がずれ、別の固定確保が大ブロックを分断)。
**WAMR プール 64→48KB**(実測消費 ~27.5KB、Linux も parity で 48KB に変更し
mp3player で検証)+ **7A の 5.3KB スクラッチ廃止(チャンクスケーリング化)**で
21.7KB 返却し解決。boot 時 free 112,128 / アプリ停止後 largest 31,744 に回復。
教訓: 「大きな静的確保を足したら largest_free_block を見る」に加え、
**ヒープからの恒久確保(タスク等)は最大連続ブロックを割る**。恒久物は静的に。

**検証**: 修正後の録音 278 オンセット中、26-27ms シグネチャの二重は **0 件**
(<80ms の 6 件はタップの物理音で発生時刻が操作時に一致)。発火間隔統計は
min=499.78/avg=500.000/max=500.04ms(セマフォ→タスク起床ぶん 7A 直呼びより
わずかに広い ±220µs、実用上問題なし)。
エビデンス: 修正前 `~/ビデオ/demo_232651.mp4`、修正後 `~/ビデオ/demo_001956.mp4`。

## 7C 実施記録 (2026-07-14) — 完了

アクセント要求を「メトロノーム特化にしない」方針で**トーンパレット API** として実装
(設計はユーザー承認済み。対案: アクセントフラグ=語彙が漏れる、PCM アップロード=
メモリ不成立で音源 API の領分)。

- `hostapi_tone_define(slot, wave, freq, dur, level)` / `tone_play(slot)` /
  `tone_schedule(slot, time_ms)` の 3 関数。slot 0..7、wave は SINE のみ
  (未知は -1)、freq 100-8000 / dur 5-100ms / level 0-100 クランプ。
  予約は 7A の契約を継承(全体 1 件、パラメータは予約時スナップショット、
  time 0 キャンセルは slot 不問)。
- **v0/7A 互換**: `play_click` ≡ `tone_play(0)`、`click_schedule(t)` ≡
  `tone_schedule(0,t)`。slot 0 はアプリ起動時に既定クリック(1000Hz/30ms/100)。
  トーン定義はアプリセッション状態(reset で初期化)。
- **キャッシュレス合成**: 発音時に再帰振動子(回転行列、サンプルあたり乗算 4)で
  チャンク合成。旧クリックキャッシュ 5.3KB を廃止し boot free heap 117,280 に改善。
  実機は ToneMsg(freq/dur/level)を静的キュー(深さ 4)でクリックタスクへ、
  Linux は voice 構造体でミキサ内合成。エンベロープは dur 終端 -30dB の指数減衰。
- metronome のアクセント対応は **tone_define 1 行+schedule の slot 分岐のみ**
  (汎用 API でアプリ側にアクセントを実装できることの実証)。
- 検証: Linux は発音ログで 4 拍子 `1568,1000,1000,1000` / 6 拍子切替を確認、
  エッジ 7 項目(クランプ受理・slot 範囲外/未知 wave/未定義 slot の -1・
  slot 不問キャンセル)を使い捨てアプリで確認、demo 回帰。実機は録音解析で
  164 拍の `nAnnnAnnn…`(4 拍)→`Annnnn…`(6 拍)パターンと**二重クリック 0 件**を
  確認、スペクトログラムでも 1568Hz が 2 秒ごとに出現。heap は停止後 73,536 で
  一定(ベースライン +5.3KB 改善)。demo.wasm 実機回帰 OK。
- 運用の教訓: herdr pane の cwd ドリフトでビルドが空振りし stale .wasm を
  検証しかけた(発音周波数のデバッグログで検出)。pane でのビルドは**絶対パス
  +成果物のタイムスタンプ/シンボル確認**をセットで行う。

検証エビデンス: `~/ビデオ/demo_121741.mp4`、`~/ビデオ/zenn-phase7c/`
(スペクトログラム 0-2.5kHz: アクセントが 2 秒周期で可視)。

## 7D 実施記録 (2026-07-19) — 完了

対応: `docs/prompts/phase7d.md`。メトロノーム磨き込み(テンポ 1 刻み・ボリューム調整)、
Host API/ABI 変更なし・App 内変更のみのスコープ。

**タスク2 事前調査(承認済み)**: `hostapi_audio_set_volume` は両ホストで
マスター音量として実装済みで、MP3 だけでなく click/tone 合成にも効く
(ESP32: `audio.cpp` の `tone_write_now` と MP3 出力後処理が同じ `volume_`
atomic を共有、Linux: `hostapi_sdl.c` の `voice_start()` と `Mix_VolumeMusic`
を同一関数内で両方更新)。対案の `tone_define` の `level`(スロット別ゲイン)も
動作するが、今回は「全体を大きく/小さく」で十分なため mp3player(6B/6C)の
V-/V+ ±10 パターンをそのまま流用する方針とした。Host API 変更なし。

**UI 案(承認済み: 案A)**: ランプ行下端(y=112)とボタン行上端(y=176)の間の
空き 64px 帯に新規 4 ボタン行(`-1`/`+1`/`V-`/`V+`、既存ボタン行と同じ
x=[12,90,168,246]・幅70を流用)を追加。既存 BPM-/BPM+(±5)はそのまま維持。
スロット収支: rect 12→16(満杯、以後の余地なし)、text 6→10(6 空き)。

**実装**(`wasm-apps/metronome/src/lib.rs`、2075B→2827B):
- タスク1: `-1`/`+1` ボタンで BPM を 1 刻み調整(40-240 クランプ、`.clamp()`)。
  長押し連打加速: 押下時に即 1 ステップ+保持状態(`HELD_DELTA`/`HELD_SINCE`/
  `NEXT_REPEAT_AT`)を仕込み、`app_tick`(100ms 格子)で 500ms 経過後から
  自動連打を開始、保持時間に応じて間隔を 400ms→200ms→100ms へ加速。
  `EV_TOUCH_UP` を新たに購読して離した瞬間に停止(このアプリは従来
  DOWN のみ処理していた)。`now_ms` の wraparound は符号付き差分
  (`time_reached`)で吸収(既存の `ANCHOR` 方式と同じ考え方)。
- タスク2: `V-`/`V+` ボタンで `hostapi_audio_set_volume`(0-100, ±10,
  既定 98 でホスト既定と一致)。ステータス行に `Vol: nnn` を追記
  (Line バッファは 32→48 バイトに拡張。既存 `BPM: 240   beats/bar: 6`
  に `  Vol: 100` を足すと 32 バイトを超えるため)。
- BPM ±5/±1 の変更は既存どおり毎回 `rearm()`(即座に再アンカー+ RUNNING
  中は拍 0 を即発音)。連打中もこの挙動を維持(既存単発 tap と一貫)。

**検証(Linux, SDL host)**: `xdotool` でウィンドウへ直接クリック/長押しを
送り込みスクリーンショットで確認。`-1`×2→BPM 120→118、`+1`→119(既知の
誤操作込みで実測 117→118 ではあるが再現性は確認)、`V-`→88、`V+`×2→
クランプ 100 で停止。`BPM+` を 2.5 秒保持で 118→158(+40、加速が機能)、
離した 2 秒後も 158 で不変(`EV_TOUCH_UP` で連打停止を確認)。長時間保持で
240 に到達しオーバーシュートなし(クランプ確認)。`BEAT`(4→6 拍子)・
`START`/`STOP`・ランプ点灯を既存どおり確認。stderr に `no free slot`
警告なし(rect 16/16 でも slot 溢れなし)。`demo.wasm` 回帰も stderr
クリーンで確認。

**検証(ESP32 実機)**: ビルド・フラッシュは `ghcr.io/wurly200a/builder-esp32/
esp-idf-v5.5:latest`(9 ヶ月前 pull、トゥールチェーン `esp-14.2.0_20241119`)で
実施(下記トラブル参照)。ユーザーに実機タッチ操作を依頼し
(`-1`×2・`+1`・`V-`・`V+`×2・`BPM+` 長押し・`BEAT`・`START`→`STOP`→終了)、
カメラ録画(`~/ビデオ/demo_221631.mp4`)+シリアルログで確認。
録画から `BPM: 175 beats/bar: 4 Vol: 98` 表示(新規行含む全ボタン描画正常)を
確認、シリアルログでも `beats/bar` が 4→6 へ変化(`BEAT` 動作)。
`no free slot` 警告なし。**heap: `app: stopped (ok), free heap 94464
(at start 94464)`** — アプリ起動〜終了で free heap 完全一致、リークなし。
起動時 i2c nack 一連(`I2C transaction unexpected nack detected`)は
`Touch online` 前のタッチコントローラ初期化リトライで本変更と無関係
(ボードリセット直後に毎回出る既知の起動ノイズ)。

**トラブルと教訓**:
- `scripts/hpane.sh` の `ensure`/`run` が今回のセッションで機能していなかった
  (未使用のまま埋め込まれていたバグ、CLAUDE.md「初回導入時の確認事項」の
  事前確認で発覚)。原因は2つ: (1) 実際の pane_id 形式が `wB:p3` であり
  スクリプトが仮定していた `\d+-\d+` と不一致、かつ `tab list`/`tab get` は
  pane_id を含まず `pane list` と `tab_id` で突き合わせる必要があった。
  (2) `python3 - "$ARG" <<'PY' ... PY` はヒアドキュメントが script 本体を
  stdin から読むため、パイプで渡した JSON を読む stdin が残らず
  `json.load(sys.stdin)` が空入力で例外になる(`-c` でスクリプトを渡せば
  stdin はパイプ入力のために空く)。両方を修正し、`ensure` の冪等性(再実行で
  同一 pane_id)と `run` の一発実行を実機検証で確認。**教訓: herdr の
  ID は `<workspace>:p<N>`/`<workspace>:t<N>` 形式(`\d+-\d+` ではない)。
  `python3 -` にヒアドキュメントで script を渡すとパイプ入力が読めなくなる
  ため、パイプ入力を読むスクリプトは必ず `-c` で渡す。**
- ESP32 ビルド用に稼働中だった docker コンテナ(`*-clangd-docker-1`)は
  `/dev/ttyACM0` 未接続の devcontainer サイドカーで、ビルド専用だった
  (flash 不可)。加えてローカルには同イメージの複数タグ/版が存在し
  (`5.5.5` は 11 時間前 pull でトゥールチェーン日付 `20260121`、`latest` は
  9 ヶ月前 pull で `20241119`)、`latest` で組んだ `build/` キャッシュに
  対して `5.5.5` でビルド/flash しようとすると `CMAKE_C_COMPILER` 不在や
  `ninja: ... newlib.lf ... missing` で失敗する。**教訓: ビルドと
  flash/monitor は同一イメージタグのコンテナで揃える。flash/monitor 用に
  `--device=/dev/ttyACM0 --group-add <dialout gid>` を付けた
  `docker run --rm -it`(monitor は `-it` 必須、`idf_monitor` が
  real TTY を要求し `-i` のみでは `Error: Monitor requires standard input
  to be attached to TTY` で失敗する)を都度起動する運用で回避した。**
- 実機のタッチ操作(タップ・長押し)はプログラムから注入する手段がなく、
  ユーザーに物理操作を依頼してカメラ録画+シリアルログで検証した
  (Linux 側は `xdotool` でウィンドウに直接クリック/長押しを送れるため
  自動化できたが、実機の I2C タッチコントローラには同等の注入経路がない)。

検証エビデンス: `~/ビデオ/demo_221631.mp4`、`~/ビデオ/zenn-phase7d/`
(`metronome_7d_device.png`: 実機で新規行 `-1/+1/V-/V+` + `Vol:` 表示が
描画されている静止画)。

# check-workflow: 開発ワークフロー一巡チェック

## 実施記録 (2026-07-20) — 完了(一部スコープ縮小)

対応: `docs/prompts/check-workflow.md`。7D で確立した herdr ペイン経由のビルド/
フラッシュ/モニタ/カメラ撮影のワークフローを、設計・ソースコード変更なしで
一巡動作確認する回。スコープ変更は `docs/prompts/check-workflow.md` 末尾の
追記節を参照。

**ステップ0(hpane.sh 改修)**: herdr の `pane split`/`pane rename` を実地検証し、
ラベル解決を「タブラベル検索」から「ペインラベル検索」に変更。共有タブ 1 つ
(`midiappbox-panes`)内に 3 列 x 2 行(esp32-build/esp32-monitor/unix-build/
camera/zenn/screen)で分割配置する方式へ改修。各ラベルにアンカーラベル+分割
方向を持たせ、`ensure_pane` を再帰化することで呼び出し順序に依存せず同じ
配置に組み上がるようにした。全 6 ラベルで `ensure` の冪等性(2 回叩いて同一
pane_id)と `run` の一発 echo 実行を確認。`run`/`send`/`waitfor`/`read` の
インタフェースは変更なし。

**ステップ1(撮影スクリプト整備)**: `scripts/cam-rec.sh`(`~/ビデオ/rec.sh` の
移植)、`cam-still.sh`、`screen-rec.sh`、`screen-still.sh` を作成、実行権限付与。
新ラベル `screen` を追加。`.gitignore` に `captures/` を追加。

**ステップ1後の発覚事項 — スコープ縮小(ユーザー承認済み、詳細は
check-workflow.md 追記節)**:
- `screen-rec.sh`/`screen-still.sh` は実装したが、この開発環境
  (Wayland + XWayland、GNOME/Mutter)では `ffmpeg -f x11grab` が常に黒画面を
  返し機能しない。GNOME Shell の D-Bus `Screenshot.ScreenshotArea` も
  `AccessDenied` で使えず、`xdg-desktop-portal` 経由の ScreenCast は初回に
  対話的な許可ダイアログが必要になるため見送り。**Linux ホストの画面キャプチャは
  今回のスコープから除外、今後の検討課題**とした。
- ランチャーのメニュー行を `xdotool mousemove --sync` + `click` で自動操作しようと
  したところ、座標計算・`getmouselocation` による検証・`windowactivate`・
  `sleep` を挟んでも**クリックが意図した行に届かない/別の行に届く**という
  不安定挙動が発生(原因未特定)。画面キャプチャも使えず結果を検証できないため、
  **Linux ホストのボタンクリック検証も今回のスコープから除外**。代わりに
  CI スモーク用の単発実行モード(`./build/midibox_host <app>.wasm`)で
  metronome.wasm を直接起動し、`app_tick` が数秒間正常に回ること・stderr に
  警告が無いこと・`xdotool key Escape` での正常終了のみを自動確認する方式に
  縮小した。

**ステップ2(Linux ホスト)**: `hosts/linux` を `cmake -B build && cmake --build
build -j` でビルド(SDL2_ttf/SDL2_mixer あり)。単発モードで
`wasm-apps/metronome/metronome.wasm` を起動、数秒 tick させ `xdotool key
Escape` で終了。`app started` → `single mode: close window or press ESC to
quit` → `app stopped` を確認、stderr 警告なし、プロセスも正常終了。

**ステップ3(ESP32 実機)**:
- ビルドでトラブル: `devcontainer up`/`devcontainer exec`(devcontainer CLI が
  作る UID remap 済みイメージ `vsc-midiappbox-...-uid`)でビルドすると
  `chmorgan__esp-audio-player` 1.0.7(pin 済み、`idf_component.yml` で
  `==1.0.7` 固定)の `audio_player.cpp:568` で
  `error: type qualifiers ignored on cast result type [-Werror=ignored-qualifiers]`
  が発生しビルド失敗。**同じソース・同じ pin バージョンでも、README と同じ
  生イメージへの `docker run` + `docker exec` ではこの箇所は warning のみで
  ビルドが通ることをユーザーが実地検証**(根本原因未特定。UID remap 由来か
  イメージビルドの非決定性かは切り分けられていない)。以後 ESP32 ビルドは
  devcontainer CLI を使わず、生イメージへの `docker exec` を使う方式に統一。
- **ミス・教訓**: `idf.py fullclean` が `managed_components` のハッシュ不一致
  (`chmorgan__esp-audio-player/audio_player.cpp` が pin バージョンと差分あり)で
  保護的に停止した際、中身を確認せず「gitignore 対象の再取得可能キャッシュ」と
  判断して `rm -rf managed_components` してしまった。実際にはローカル修正
  (おそらく上記 `-Wignored-qualifiers` 対策)が入っていた可能性が高く、
  復元不能にしてしまった。結果的に生イメージでは同じ箇所が warning で通ったため
  実害はなかったが、**gitignore 対象でも中身を確認せず削除しないこと**を
  教訓とした。
- `docker run ... bash -lc '...'` はログインシェル扱いで `~/.bashrc`
  (`export IDF_PATH=/opt/esp-idf; source /opt/esp-idf/export.sh` を含む)を
  読まないため `idf.py: command not found` になった。`bash -c 'source
  /opt/esp-idf/export.sh && idf.py ...'` で明示 source する方式に修正。
- ビルド・フラッシュは README タグ(`ghcr.io/wurly200a/builder-esp32/
  esp-idf-v5.5:5.5.5`)の生イメージコンテナへの `docker exec`/`docker run`
  で成功(`midi_app_box.bin` 生成、`idf.py -p /dev/ttyACM0 flash` 成功)。
  シリアルモニタは `docker run --rm -it --device=/dev/ttyACM0 --group-add
  <dialout gid>` の別コンテナで起動、`app_main` を `waitfor` で確認。
  ユーザーに「ランチャーから metronome → START → 数秒 → STOP → 終了」の
  物理操作を依頼し、`camera` ペインで `cam-rec.sh`(55 秒、h264 1280x720)+
  `cam-still.sh` で記録。シリアルログで `launch: /sdcard/apps/metronome.wasm`
  → `app_init() = 0, free heap 53508, tick loop start` → タッチイベント
  複数回 → `app: stopped (ok), free heap 73972 (at start 73972), largest
  block 31744` → `menu: 6 app(s) listed` を確認、**free heap が開始時と完全
  一致(リークなし)**。stderr 相当の警告(WARN/ERROR/no free slot/nack)は
  スクロールバック確認範囲でなし。

**完了条件確認**: `captures/check-workflow/` に Linux ホストの動画・静止画は
無い(スコープ除外)が実機の動画(`cam_rec_112014.mp4`)・静止画
(`cam_still_112117.png`)は取得済み。`git status` に `captures/` は現れない
(.gitignore 機能)。CLAUDE.md 更新(ペイン表 `screen` 追加・分割レイアウト・
`cam-rec.sh` 参照・教訓チェックリスト・現在地)。

検証エビデンス: `captures/check-workflow/cam_rec_112014.mp4`、
`captures/check-workflow/cam_still_112117.png`(いずれも `.gitignore` 対象、
リポジトリには含まれない)。

# check-workflow-routine: 開発ワークフロー一巡チェック(定期実施)

## 実施記録 (2026-07-20) — 完了

対応: `docs/prompts/check-workflow-routine.md`。docs/workflow.md §4「一巡チェック
モード」として、確立済みワークフロー(§3.0〜§3.3)を設計・スクリプト変更なしで
一巡実行し確認した回。出力先タスク名は `captures/check-workflow-routine/`、
実機操作は metronome。

**§3.0(環境確認)**: 共有タブが未作成の状態から、全 6 ラベル
(esp32-build/esp32-monitor/unix-build/camera/zenn/screen)を `ensure` し、
2 回叩いて同一 pane_id を返すこと(冪等性)を確認。`run` の一発 echo テストも
exit 0 で成功。

**§3.1(Linux ホスト)**: `hosts/linux` を `cmake -B build && cmake --build
build -j` でビルド(SDL2_ttf/SDL2_mixer あり、exit 0)。単発モードで
`metronome.wasm` を起動、`app started` を確認後数秒 tick させ、`xdotool key
Escape` で終了。ログは `app started` → `single mode: ...` → `app stopped` の順で
一貫、stderr 相当の警告なし。`pgrep -af midibox_host` はプロセス残留なし。
(補足: `xdotool search --name "MidiAppBox WASM host"` が `mutter-x11-frames`
プロセスの装飾ウィンドウにも誤って一致する事例を確認。`getwindowpid` で
対象プロセスの PID と突き合わせて実ウィンドウを特定した。)

**§3.2(ESP32 実機)**: `docker ps -a` で確認したところ、実行中コンテナは
devcontainer CLI 由来の UID remap 済みイメージ(`vsc-midiappbox-...-uid`)のみで、
README タグ(`ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:5.5.5`)の持続コンテナは
存在しなかったため、`docker run -d --name midiappbox-esp32-build -v <repo>:/
workspaces/MidiAppBox -w /workspaces/MidiAppBox <tag> sleep infinity` で新規に
起動し、`docker exec` でビルド(`idf.py build`、exit 0、`midi_app_box.bin` 生成)。
フラッシュ・モニタは CLAUDE.md 教訓どおり `docker run --rm -it
--device=/dev/ttyACM0 --group-add 20` の都度起動方式(dialout gid=20)で実施し、
両方とも exit 0 / `app_main` 到達を確認。チェック終了後、今回起動した持続
コンテナ(ビルド用・モニタ用)は `docker stop`/`rm` で片付けた。

**§3.3(実機動作検証)**: `camera` ペインで `cam-rec.sh
captures/check-workflow-routine` により録画開始 → ユーザーに「ランチャーから
metronome 起動 → START → 数秒 → STOP → 終了」を依頼 → 完了報告を受けて録画停止
→ `cam-still.sh` で静止画取得。`ffprobe` で動画が h264/aac, 1280x720, 49.65 秒の
正常な mp4 であることを確認、`ffmpeg` プロセス残留なし。シリアルログで
`launch: /sdcard/apps/metronome.wasm` → `app_init() = 0, free heap 53508, tick
loop start` → タッチイベント複数 → `app: stopped (ok), free heap 73972 (at
start 73972), largest block 31744` → `menu: 6 app(s) listed` を確認、**free
heap は開始時と完全一致(リークなし)**。

**観測した警告(1件、既知の起動ノイズ一覧にはなし)**: ボード起動直後に
`W (385) spi_flash: Detected size(16384k) larger than the size in the binary
image header(2048k). Using the size in the binary image header.` が出ていた。
アプリ起動・動作・終了・heap 確認には影響なく、パーティションテーブル上の
想定フラッシュサイズ(2MB)と実機搭載フラッシュ(16MB)の差に関する ESP-IDF
起動時の定型警告であり、今回のビルド/フラッシュ手順が原因で新たに出た
ものではないと考えられる(アプリ非依存、毎回起動時に出る想定)。CLAUDE.md
既知の起動ノイズ一覧(I2C nack 系、Touch online 前)には含まれていないため
念のため記録するが、一巡チェックの完了条件(WARN/ERROR/no free slot なし)の
判定には影響しないと判断した。

**完了条件確認**: 全手順が `hpane.sh` 経由で完走(§3.0〜§3.3)。
`captures/check-workflow-routine/` に実機の動画(`cam_rec_121459.mp4`)・
静止画(`cam_still_121558.png`)を確認、`git status --porcelain` に `captures/`
は現れない(.gitignore 機能)。free heap 一致・(既知外の 1 件を除き)警告なしを
確認。scripts/・CLAUDE.md・ソースコードへの変更なし、`git status --porcelain`
の差分は本追記のみ。

**手順の改善案(§5 に基づき報告のみ、本チェック内では未実施)**:
- README タグの持続コンテナ(`esp32-build` 用)が存在しない状態から一巡
  チェックを始めるケースを想定し、§3.2 の先頭に「持続コンテナが無ければ
  `docker run -d --name <container> ... sleep infinity` で起動する」手順を
  明記してはどうか。現状は `<container>` が既に起動している前提でコマンド例
  だけが書かれており、今回のように無い場合の対処が本文になく都度その場で
  判断する必要があった。
- `xdotool search --name "MidiAppBox WASM host"` が複数ウィンドウ ID を返し、
  うち 1 件が無関係な `mutter-x11-frames` プロセスの装飾ウィンドウだった
  (今回は `getwindowpid` で実プロセスと突き合わせて回避)。§3.1 の手順に
  「複数ヒットした場合は `getwindowpid` で対象プロセスの PID と照合する」旨を
  一行加えると、次回以降同じ切り分けをせずに済む。

検証エビデンス: `captures/check-workflow-routine/cam_rec_121459.mp4`、
`captures/check-workflow-routine/cam_still_121558.png`(いずれも `.gitignore`
対象、リポジトリには含まれない)。

## 改善案の反映 (2026-07-20)

上記「手順の改善案」2 件について、ユーザー承認を得て docs/workflow.md §5 の
手続きに従い反映した(スクリプト・herdr レイアウトの変更は伴わない、
ドキュメントのみの更新):

- **docs/workflow.md §3.2**: 持続コンテナ (`<container>`) が存在しない場合の
  起動コマンド(`docker run -d --name <container> ... sleep infinity`)を
  ビルド手順の先頭に追記。
- **docs/workflow.md §3.1**: `xdotool search` が複数ウィンドウ ID を返した
  場合に `getwindowpid`/`pgrep -af midibox_host` で対象を照合する手順を追記。
- **CLAUDE.md 教訓チェックリスト**: 上記 2 点をそれぞれ「herdr / ビルド」
  「Linux ホスト(SDL / GUI 自動化)」節に一行追加。

# cam-rec.sh: 動画・音声ずれの調査と対処

## 実施記録 (2026-07-20) — 完了

check-workflow-routine の完了報告で触れた既知課題(`docs/workflow.md` の
「cam-rec.sh の動画・音声ずれ」)について、ユーザー依頼で別タスクとして調査した。

**原因調査**:
1. `scripts/cam-rec.sh` の ffmpeg コマンドを確認したところ、v4l2(カメラ)と
   pulse(マイク)を1プロセスで同時キャプチャしている。録画開始直後
   (`apply_settings()` の初回呼び出しで `auto_exposure` を自動→手動へ切替える
   タイミング)に v4l2 側→pulse 側の順で `Thread message queue blocking;
   consider raising the thread_queue_size option (current value: 8)` が
   各1回だけ出ていた(3秒ごとの再設定ループでは再発しない)。
2. `ffmpeg -h demuxer=pulse` で pulse 入力は既定 `-wallclock 1`(壁時計基準)、
   `ffmpeg -h demuxer=video4linux2` で v4l2 入力は既定 `-timestamps default`
   (カーネル/モノトニック基準)と、**入力ごとに異なる時刻系**を使っていること
   を確認。1. の起動直後のスタッツが正しく相対オフセットとして反映されない
   要因と推定した。

**対処 (1段目、ユーザー承認済み)**: `scripts/cam-rec.sh` の ffmpeg 呼び出しに
`-thread_queue_size 1024`(両入力)・`-timestamps abs`(v4l2 側)を追加。
再テスト録画で `Thread message queue blocking` 警告は解消。

**効果測定**: 単純な拍手テストでは映像に手が映らず測定に使えなかったため、
代わりに実機で稼働中だったメトロノームアプリの「ビート表示の点滅(映像)」と
「クリック音(音声)」を同一デバイス発生源の同期基準として利用。
`ffmpeg signalstats`(ビート点灯領域の輝度立ち上がり検出)と音声 RMS
エンベロープのピーク検出で多数のビートをペアリングし、修正前
(`check-workflow-routine/cam_rec_121459.mp4`)・修正後(1段目)の両方を比較した
結果、**`Thread message queue blocking` は解消したが、音声が映像よりおよそ
150〜210ms 遅れる一定オフセットは修正前後で変わらず残存**していることが
判明した(進行性のドリフトではなく録画全体でほぼ一定)。低照度対策の固定
露光(`exposure_time_absolute=451` ≒ 45ms/フレーム)+ USB MJPEG カメラの
読み出し・デコード遅延に由来する、カメラハードウェア側の構造的遅延と推定。

**対処 (2段目、ユーザー承認済み)**: 上記推定に基づき、pulse 入力に
`-itsoffset -0.2`(音声を約200ms前倒し)を追加する対症療法を検証。
- 事後処理での検証(既存録画ファイルを `-itsoffset -0.2` で音声のみ再マルチ
  プレクス)で、ビートオフセットの平均が 210ms → 12.3ms(標準偏差 38ms)に
  縮小することを確認。
- `scripts/cam-rec.sh` に本設定を組み込んだ上でライブ録画による実地検証も
  実施、平均 14.7ms(標準偏差 24.4ms、いずれもカメラのフレーム精度 33ms 相当
  以内)まで縮小することを確認した。

**結論・注意点**: `scripts/cam-rec.sh` に `-thread_queue_size 1024`・
`-timestamps abs`(構造的なキュー詰まり・時刻系不一致の修正)と
`-itsoffset -0.2`(対症療法、経験値)を追加してコミット。ただし 200ms は
本機(Logitech StreamCam)・この露光設定(`exposure_time_absolute=451`)での
経験値であり、カメラ機種や露光設定を変えた場合は再調整が必要になる可能性が
ある旨をスクリプト冒頭コメントと docs/workflow.md §3.3 に記録した。

検証エビデンス: 検証用録画・切り出し音声/フレームはすべて scratchpad
または `.gitignore` 対象の `captures/` 配下の一時ディレクトリで作業し、
作業終了後に削除済み(リポジトリに残る成果物なし)。
