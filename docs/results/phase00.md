# Phase 0: 調査 + 旧ルールの履歴

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

## Phase 0 調査結果 (2026-07-05)

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
