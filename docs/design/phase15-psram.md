# Phase 15 設計メモ — PSRAM 本番反映(WASM linear memory の PSRAM 移行)

対応する指示書: `docs/prompts/phase15.md` ステップ 1(承認ゲート)
実測の根拠: `docs/results/phase15.md` ステップ 0(O-1〜O-5、条件 C0/C1/C2)

---

## 1. 採用する手段 — **A 案 + `CONFIG_SPIRAM_USE_CAPS_ALLOC`**

### 1-1. 決定

`sdkconfig.defaults` に PSRAM 設定を入れるだけ。**WAMR にも `managed_components/` にも
一切手を入れない**(ゲート 5 に抵触しない)。

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

### 1-2. なぜ A 案で足りるのか(実測で確定した連鎖)

| 段 | 事実 | 出典 |
|---|---|---|
| 1 | IDF 5.5 は `CONFIG_ESP32S3_SPIRAM_SUPPORT` を `CONFIG_SPIRAM` へ改名した旧名として保持し、kconfgen は**旧名も値付きで `sdkconfig.cmake` に出力する** | `components/esp_hw_support/sdkconfig.rename.esp32s3`、C1 ビルドで `set(CONFIG_ESP32S3_SPIRAM_SUPPORT "y")` を実見 |
| 2 | WAMR 2.4.0 の `core/shared/platform/esp-idf/shared_platform.cmake` はその旧名を見て `-DWASM_MEM_DUAL_BUS_MIRROR=1` を付ける | C1 ビルドの `compile_commands.json` に 37 件 |
| 3 | `espidf_memmap.c: os_mmap()` は同マクロが立つと非実行領域の確保 caps を `MALLOC_CAP_8BIT` → `MALLOC_CAP_SPIRAM` に切り替える | ソース |
| 4 | linear memory はプールではなく `os_mmap()` から取られる(`WASM_MEM_ALLOC_WITH_USAGE` の既定は 0) | `wasm_memory.c: wasm_allocate_linear_memory()` |
| 5 | **実機で `memory_data=0x3c101820`(PSRAM)** | C1/C2 の実測。C0 は `0x3fcf02e8`(internal) |

### 1-3. なぜ `CAPS_ALLOC` で、`USE_MALLOC` ではないのか

| 観点 | C1 `CAPS_ALLOC` | C2 `USE_MALLOC` |
|---|---|---|
| INTERNAL largest(アプリ実行中) | **40,960** | 31,744 |
| INTERNAL free | 88,480 | 87,703 |
| 大領域(0x3fcc….–0x3fce9710)の最大連続 | 42,280 | 9,472 |
| `malloc()` の戻り先 | 常に internal | 16KB 超は PSRAM |

1. **largest free block が大きい。** `USE_MALLOC` は
   `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`(既定 32,768)が大領域から予約プールを
   切り出すため、大領域が 9,472 まで断片化する。
2. **予測可能性。** `CAPS_ALLOC` は `malloc()` が PSRAM を返さないので、
   **IDF ドライバ内部の一般 `malloc` が意図せず PSRAM へ流れる経路がない。**
   PSRAM に置くものは我々が `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` で明示した
   ものだけになる(LVGL の例外は §3 で扱う)。
   `USE_MALLOC` は「16KB を境に配置が変わる」という暗黙のルールを全コードに課す。
3. **§9 の方針に対して安全側。** どちらでも L0/L1 は静的 BSS なので internal に
   留まるが、`CAPS_ALLOC` は将来ヒープ確保を足したときも既定で internal に落ちる。

### 1-4. B 案(`.wasm` バッファの明示 PSRAM 化)は**本フェーズでは採らない**

`read_wasm_file()` の `malloc(size)` は `CAPS_ALLOC` では internal に残る。
現状の最大は midi_loopback の 5,947 B で、C1 の internal largest 40,960 に対して
十分な余裕がある。**効果がないものを今入れる理由がない。**

ただし `read_wasm_file()` の上限チェックは 512KB であり、アプリが 30KB 級になると
internal の最大連続ブロックと競合し始める。**再検討のトリガを明記しておく:
いずれかの `.wasm` が 16KB を超えた時点で B 案を実施する。**
(F6 のとおりバッファは書き込み可能である必要があるので、
`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` に置き換えるだけでよい。読み取り専用
マップにはしない。)

### 1-5. 副作用

| 副作用 | 内容 | 扱い |
|---|---|---|
| SD が SDSPI 固定になる | PSRAM 有効時は SDMMC プローブから戻らない(Phase 12 E5)。プローブを飛ばすと常に SDSPI 11.43MHz | **もう memory 上の破綻にはならない。** Phase 12 で SDSPI が危険だったのは largest が 15,360 に落ちて linear memory が取れなくなるためだが、linear memory は internal から出た。ステップ 4 で真因を切り分ける |
| `esp_get_free_heap_size()` が PSRAM 込みになる | 回帰の指標が壊れる | §4 で対処 |
| LVGL 描画バッファが PSRAM へ移る | 指示書のスコープ外項目に自動で踏み込む | §3 で対処(L-a 採用 + 条件付きフォールバック) |
| 内部 RAM が管理領域ぶん減る | 大領域が 163,600 → 156,128(−7,472 B) | 許容。差し引きで internal free は +58,560 |

---

## 2. PSRAM に置くもの / 置かないもの

**§9(メモリ配置方針)は変更しない。** L0 キュー・テンポマップ・L1 の状態・
DMA バッファ・タスクスタックは internal 固定のままである。

| 対象 | 配置 | 機構 | 根拠 |
|---|---|---|---|
| **WASM linear memory** | **PSRAM** | `os_mmap` が `MALLOC_CAP_SPIRAM` | 本フェーズの目的そのもの |
| LVGL 描画バッファ(19,200 B ×2) | **PSRAM**(暫定) | `MALLOC_CAP_DEFAULT` に PSRAM が含まれる | §3。条件を満たさなければ internal へ戻す |
| L0 キュー(4,096 B) | internal | `shared/seq_core.c` の静的 BSS | §9。リンカが internal DRAM に置く |
| テンポマップ / 拍子マップ | internal | 同上 | 同上 |
| L1 の状態(`s_seg_*` 等) | internal | 同上 | 同上 |
| WAMR プール(49,152 B) | internal | `wasm_runtime.cpp` の静的 BSS | Phase 7B-fix。module/instance/exec_env はここから |
| `.wasm` バッファ | internal | `malloc()`(`CAPS_ALLOC` では internal) | §1-4。16KB 超で再検討 |
| クリックタスク / シリアルコンソールのスタック | internal | `xTaskCreateStatic` + 静的 BSS | Phase 12 |
| `xTaskCreate` 組のタスクスタック | internal | **IDF の既定**。`FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` は `xTaskCreateStatic` に外部バッファを渡すことを許すだけ | Kconfig の help を実見 |
| I2S / SPI の DMA ディスクリプタ | internal | `MALLOC_CAP_DMA` | F8 |

---

## 3. LVGL 描画バッファ — **L-a(PSRAM を受け入れる)を採用。ただし条件付き**

### 3-1. なぜ判断が要るのか

`esp_lvgl_port` は `disp_cfg.flags.buff_dma = 0` / `buff_spiram = 0` のとき
`buff_caps = MALLOC_CAP_DEFAULT` で確保する。`MALLOC_CAP_DEFAULT` には PSRAM が
含まれるため、**PSRAM を有効にすると自動的に PSRAM へ移る**(`CAPS_ALLOC` でも
`USE_MALLOC` でも同じ)。指示書はこれをスコープ外としているので、黙って通せない。

**L-a を採用する。** internal を 38,400 B 節約でき、C1 の internal largest 40,960 は
この効果込みである。C0〜C2 の実測で描画由来の WARN/ERROR は 0 件だった。

### 3-2. パネルへの転送経路とバウンスバッファ — **既定では internal にバウンスする**

構成:

```
LVGL flush → lvgl_port_flush_callback → esp_lcd_panel_draw_bitmap
  → esp_lcd_panel_io_spi (ST7789, SPI2_HOST, pclk 40MHz, trans_queue_depth 10)
  → spi_master (SPI_DMA_CH_AUTO, max_transfer_sz = 240*80*2 = 38,400)
```

- **RGB/DPI パネルではないので、`esp_lcd` 側のバウンスバッファ機構
  (`bounce_buffer_size_px`)は存在しない。** `on_bounce_frame_finish` の参照は
  `avoid_tearing` の RGB 分岐にあり、本構成では通らない。
- **代わりに `spi_master` がバウンスする。** `esp_lcd_panel_io_spi.c` は
  `color_in_psram && flags.psram_dma_direct` のときだけ `SPI_TRANS_DMA_USE_PSRAM` を
  立てる。`esp_lcd_panel_io_spi_config_t.flags.psram_dma_direct` は
  `display.cpp` で `io_config = {}` としているため **0**。したがって
  `spi_master.c: setup_dma_priv_buffer()` は

  ```c
  bool use_psram  = is_ptr_ext && (flags & SPI_TRANS_DMA_USE_PSRAM);   // false
  bool need_malloc = is_ptr_ext ? (!use_psram || ...) : ...;           // true
  ...
  uint32_t *temp = heap_caps_aligned_alloc(alignment, align_len, MALLOC_CAP_INTERNAL);
  memcpy(temp, buffer, len);
  ```

  を通り、**フラッシュ 1 回ごとに転送サイズぶんの internal DMA バッファを一時確保して
  memcpy する。**
- **サイズ**: フラッシュ領域のバイト数。LVGL のバッファが `240 × 40 px × 2 B =
  19,200 B` なので **最大 19,200 B**(`spi_trans_max_bytes` = 38,400 未満なので分割なし)。

**これは本フェーズの目的を直接損なう。** internal を 38,400 B 空けた見返りに、
描画のたびに最大 19,200 B の internal を一時的に取り返されるからである。

### 3-3. 対処 — `psram_dma_direct = 1` を立てる

`display.cpp` に 1 行:

```c
io_config.flags.psram_dma_direct = 1;  // 色バッファが PSRAM のとき DMA を直結する
```

これで `use_psram = true` になり、`mem_cap` は `MALLOC_CAP_SPIRAM` に変わる。
アドレスと長さが `dma_align_tx_ext` に揃っていれば `need_malloc` が false になり
**完全なゼロコピー**、揃っていなくてもバウンス先は PSRAM になり **internal は消費しない**。
SPI2_HOST を使っているので「SPI3 は外部メモリ非対応」の制約にも当たらない。

どちらに落ちたかは実測で確かめる(§3-4 の計測項目)。

### 3-4. 追加する常設ログと計測項目

1. **描画バッファのアドレスを起動ログに出す(常設)。**
   `Display::init()` の `lvgl_port_add_disp()` の直後に、
   `lv_display_get_buf_active(disp_)` の `data` / `data_size` と、
   `lvgl_port_add_disp()` 前後の INTERNAL / SPIRAM free の差分を出す。
   前者でアドレス(0x3c…=PSRAM / 0x3fc…=internal)を、後者で
   **2 面ぶんが本当に PSRAM から出たか**を確定する
   (`lv_display_get_buf_active` は片面しか返さないため、差分と併せて見る)。
2. **T-2 に「LVGL フラッシュ 1 回あたりの所要時間」を追加する。**
   `app_tick` と同じ形式(min / avg / p50 / p95 / p99 / max、数千サンプル)。
   **判定は絶対値ではなく記録が目的。** ただし
   **`app_tick` の最大実行時間が 100ms の tick 周期を脅かす水準なら報告して止める**
   という既存の条件を、このフラッシュ時間にも適用する
   (フラッシュは LVGL タスク優先度 4 で回り、WASM アプリのスレッドは優先度 5。
   フラッシュが長引けば `app_tick` の実行時間に現れるので、両者を並べて見る)。
   `psram_dma_direct` の 0 / 1 を両方測り、採用値を実測で決める。
3. **ステップ 3 にカメラによる目視確認を入れる**(`docs/workflow.md` §3.3)。
   **ティアリング・描画欠け・色化けは WARN/ERROR に出ない。**
   自動回帰では絶対に捕まらないので、人間の目で見るしかない。
   確認内容: ランチャーのスクロール、metronome の BPM 表示更新、
   mp3player の再生中の画面。

### 3-5. フォールバック — **L-b(`buff_dma = true`)へ切り替える条件**

次のいずれかに当たったら L-b に切り替える:

- **T-1(metronome の MIDI クロック)が Phase 13 の絶対値目標を満たさない。**
- **T-2 の悪化が許容外**、すなわち `app_tick` の最大実行時間が 100ms 周期を
  脅かす水準に達する、または §3-4 の目視確認でティアリング・描画欠けが出る。

切り替えは `display.cpp` の 1 行:

```c
disp_cfg.flags.buff_dma = true;   // MALLOC_CAP_DMA が付き PSRAM は対象外になる
```

**切り替えても WASM は困らない。** 根拠:

1. **linear memory の確保先は LVGL と無関係に決まる。** `os_mmap` が
   `MALLOC_CAP_SPIRAM` を使うのは `WASM_MEM_DUAL_BUS_MIRROR` によるもので、
   LVGL バッファがどこにあっても変わらない。**WASM が internal の最大連続ブロックを
   奪い合う関係は、A 案の時点で既に切れている。**
2. **L-b で internal largest は C0 相当(アイドル 31,744)まで戻るが、それで困る
   利用者がもういない。** C0 でその値を食い潰していたのは linear memory
   (16,496〜17,696 B)だけであり、それが PSRAM へ出た。残る internal の連続確保は
   `.wasm` バッファ(現状最大 5,947 B)と各ドライバの小口だけで、
   31,744 に対して桁が違う。
3. **T-3(引き上げた `.wasm`)にも影響しない。** 引き上げぶんはすべて PSRAM 側に
   乗るので、internal largest がいくつでも起動可否は変わらない。

つまり L-b は「internal の空きが 38,400 B 減るだけ」で、**本フェーズの成果は
1 バイトも失われない。** これが L-a を暫定採用として試せる理由である。

---

## 4. 回帰の指標 — **ファーム側を INTERNAL 基準にし、PSRAM を別列で足す**

### 4-1. 問題

PSRAM を有効にすると `esp_get_free_heap_size()` と
`heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)` が PSRAM を含み、
free heap 8,406,315 / largest 8,257,536 になる。`EXPECT_LARGEST=31744` と一致せず
**全アプリ FAIL 表示**(実質は 5 アプリ全部起動・WARN/ERROR 0 件で PASS)。
このままだと T-4 の 20 回連続とステップ 5 の全アプリ回帰が
「毎回 FAIL と出るが実は PASS」になる。

### 4-2. 決定 — 直すのはファーム側

**`esp_get_free_heap_size()` が 8.4MB を返すのはバグではなく、指標の意味が壊れた
ということである。** 回帰が見たいのは「internal が枯れていないか」「リークしていないか」
であって、8MB の PSRAM を足した合計値はどちらも表さない。
ファームのログ出力そのものが誤った量を報告している状態なので、直すべきはファーム。
スクリプト側で読み替えるのは、壊れた計器を残したまま目盛りだけ書き換えることになる。
指示書のステップ 2 が「internal free・largest を常設で出す」と要求しているので、
そこに合流する形でもある。

### 4-3. **2 値ではなく 4 値にする**(見落としの補い)

INTERNAL に置き換えるだけだと、**PSRAM 側のリーク監視が完全に消える。**
linear memory は今 PSRAM から出ているので、アプリの load/unload を繰り返して
PSRAM が漏れても INTERNAL 基準の回帰は 20 回とも PASS を返す。
**これは本フェーズの変更で新しく生まれた盲点**であり、放置すると
「数十回起動すると落ちる」という形でしか気づけない。

出力する 4 値(`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 系統と `MALLOC_CAP_SPIRAM` 系統):

```
app: stopped (ok) free_int=<N> largest_int=<N> free_psram=<N> largest_psram=<N> (at start free_int=<N> free_psram=<N>)
```

`heap` シリアルコマンドの応答も同じ 4 値に揃える。

### 4-4. 判定の設計 — 固定値一致をやめる

**`EXPECT_LARGEST` を新実測値に差し替えるだけにしてはいけない。**
C1 の `largest_int` は 40,960 だったが、これはアプリごとに違いうる
(linear memory は出て行っても、`.wasm` バッファやアプリ固有の native 確保は
internal に残る)。単一の期待値を全アプリに当てると、
「毎回 FAIL だが実は PASS」を別の形で作り直すことになる。

**2 つの目的を分離する:**

| 目的 | 判定 |
|---|---|
| **余裕の監視** | `largest_int >= MIN_LARGEST_INT` かつ `free_int >= MIN_FREE_INT`。PSRAM 側も `free_psram >= MIN_FREE_PSRAM` の下限しきい値 |
| **リーク検出** | **同一アプリの起動→停止を N 回繰り返して、値が単調減少しないこと。** PSRAM は 8MB あるので下限しきい値よりこちらが実質的な監視になる |

**この 2 つを混ぜていたのが元の設計の弱点である。**

### 4-5. 実装の分割(ゲート 4 の切り分け)

| # | 変更 | ゲート | 内容 |
|---|---|---|---|
| **4a** | ファームのログ 4 値化 | **不要**(ステップ 2 の一部) | `wasm_runtime.cpp` の `app: stopped` 行と `serial_cmd.cpp` の `heap` 応答。**これだけ先に入れれば、4b が承認待ちの間もログは正しい値を出している** |
| **4b** | `device-regress.sh` のパース + `device-regress.conf` のスキーマ | **要**(ゲート 4) | 固定値一致 → 下限しきい値。`EXPECT_LARGEST` を `MIN_LARGEST_INT` / `MIN_FREE_INT` / `MIN_FREE_PSRAM` に置き換える |
| **4c** | 反復時の非減少判定 | **要**(ゲート 4) | 同一アプリ N 回反復の追加 |

**4a と 4b を別コミットにする。**
4b は 4a のログ行書式に依存するので、4a → 4b の順で入れる。

**4c は次フェーズへ回すことを提案する。** 本フェーズは PSRAM 反映本体・T-1〜T-4・
ステップ 4 の切り分けを抱えており、回帰フレームワークの作り直しまで載せると
どちらも中途半端になる。その場合、
**「PSRAM のリーク監視が未実装である」ことを `docs/status.md` に明記する**
(本フェーズの変更で生まれた既知の穴なので、記録が残っていないと忘れる)。

---

## 5. `CONFIG_SPIRAM_XIP_FROM_PSRAM` — **採らない(判断のみ記録し、実施しない)**

指示書が許した「判断だけ書いて実施しない」を選ぶ。理由:

1. **T-1 / T-2 の因果が切り分けられなくなる。** XIP は `.text` / `.rodata` を
   PSRAM へ移し、**コード実行速度そのものを変える。** 本フェーズの合否は
   T-1(MIDI クロックの絶対値目標)で決まるので、linear memory の移動と
   コード配置の変更を同時に入れると、悪化したときにどちらが原因か言えない。
2. **今フェーズの動機に対して効かない。** XIP の利点は「flash 操作中もキャッシュが
   無効化されないので `IRAM_ATTR` を減らせる」であり、狙うのは internal RAM の節約。
   だが internal largest は既に 31,744 → 40,960、internal free は 29,920 → 88,480 に
   なっている。**今の逼迫水準では投資に見合わない。**
3. **ステップ 4(SDMMC と PSRAM の共存)と干渉する。** SDMMC プローブが
   PSRAM 有効時に戻らない原因はまだ切り分けていない。キャッシュ挙動を変える設定を
   同時に入れると、その切り分けが濁る。

**再検討のトリガ**: `IRAM_ATTR` を増やす必要が出たとき、または
internal largest が再び 20KB を切ったとき。

---

## 6. 検証用アプリの引き上げ幅(T-3)

### 6-1. 引き上げる操作 — `-zstack-size`(`--initial-memory` ではない)

O-5 のとおり、WAMR の `WASM_ENABLE_SHRUNK_MEMORY`(既定 1)が memory セクションの
宣言を無視して `num_bytes_per_page = align8(__heap_base)` / `init_page_count = 1` に
潰すため、**`--initial-memory` を上げても linear memory は増えない。**
実際のサイズは

> **linear memory = align8(`__heap_base`) + 8,192**(instantiate の app heap)

で決まり、C0 の 5 アプリすべてがこの式に 1 バイトの狂いもなく一致した。
`__heap_base` を上げる操作が `-zstack-size` である。

### 6-2. 対象と幅

**対象は `touch_demo` 1 本。** 理由: 新規アプリを足さない(パーティション逼迫の回避)、
かつ T-1 の測定対象(metronome)と計測系の妥当性確認(seq_smoke)を汚さない。

| 段 | `-zstack-size` | 予想 linear memory | 意味 |
|---|---|---|---|
| 基準 | 8,192(現行) | 16,496 | C0 と同じ |
| **T-3 本番** | **65,536** | **約 73,840** | **C1 の internal largest 40,960 を超える。** internal からは原理的に取れないので、PSRAM で動いていることの決定的な証明になる |
| 上限探索 | 256KB → 1MB → 4MB | 〜約 4MB | 「どこまで引き上げると失敗するか」を 1 回だけ探る |

上限探索では、失敗が **PSRAM の枯渇**(`allocate linear memory failed`)なのか
**別の要因**(`memory_instantiate` の `default_max_page` 判定など)なのかを
ログで区別して記録する。

### 6-3. 引き上げたまま main に残すか — **残さない**

T-3 と上限探索を終えたら `touch_demo` の `-zstack-size` は **8,192 に戻す**。
理由: 回帰の基準値(free heap / largest)を 1 アプリだけ特異にしても
継続的な価値がない。

代わりに、**`memory_data` のアドレスを出す常設ログを恒久の見張りにする。**
将来なにかの拍子に linear memory が internal へ戻ったら
(WAMR の更新で旧 config 名の参照が消える、IDF 6 でリネームが撤去される、等)、
アドレスの先頭バイトが `0x3fc…` に変わるので回帰ログで即座に気づける。
**これは「largest free block を見る」より正しい見張りである**
(largest は WASM の可否を表さない、というのが本フェーズ最大の教訓)。

---

## 7. 実施順序

| # | 内容 | ゲート |
|---|---|---|
| 1 | `sdkconfig.defaults` に PSRAM 設定(値ごとに根拠コメント) | — |
| 2 | SDMMC プローブの扱いを恒久化(ステップ 4 の結論が出るまでは迂回を維持) | — |
| 3 | 常設ログ: `memory_data` アドレス / サイズ、INTERNAL・SPIRAM の free / largest、LVGL 描画バッファのアドレス | — |
| 4 | **4a**: `app: stopped` と `heap` の 4 値化 | — |
| 5 | `display.cpp` に `psram_dma_direct = 1` | — |
| 6 | **4b**: `device-regress.sh` / `.conf` を下限しきい値へ | **ゲート 4** |
| 7 | T-1 / T-2(`psram_dma_direct` 0・1 の両方)/ T-3 / T-4 | — |
| 8 | ステップ 3 のカメラ目視確認 | — |
| 9 | ステップ 4(SDMMC と PSRAM の共存の切り分け) | — |
| 10 | ステップ 5(回帰・文書化)。4c を次フェーズへ回すなら `docs/status.md` に穴を明記 | — |

**Host API / ABI は一切変更しない**(ゲート 3)。
