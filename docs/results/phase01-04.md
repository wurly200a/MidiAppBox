# Phase 1〜4: WAMR動作・ホストAPI・Linuxホスト・計測(詳細計画)

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

