# Phase 15 実施記録 — PSRAM 本番反映(WASM linear memory の PSRAM 移行)

対応するフェーズ指示書: `docs/prompts/phase15.md`

前提として通読したもの: `CLAUDE.md` / `docs/workflow.md` / `docs/lessons.md` /
`docs/architecture.md` §9・§11-2 / `docs/results/phase10.md` P10-4 /
`docs/results/phase12.md` / `docs/results/phase13.md`。

生データ: `captures/phase15/`(.gitignore 対象)。

---

## ステップ 0: 事実確認(実装なし・承認ゲート)

### 検証用の一時コード(フェーズ完了時に削除する)

| マクロ | 場所 | 内容 |
|---|---|---|
| `PHASE15_STEP0` | `src/CMakeLists.txt` で定義、`wasm_runtime.cpp` で使用 | instantiate 直後に linear memory の先頭アドレス・サイズ、INTERNAL/SPIRAM の free・largest、INTERNAL ヒープの**領域別**内訳をログ出力する |
| `PHASE15_NO_SDMMC_PROBE` | `src/CMakeLists.txt` / `sdcard.cpp` | SDMMC プローブを飛ばして SDSPI 直行(Phase 12 E5 の迂回。PSRAM 有効ビルドの起動に必要) |

`heap_caps_print_heap_info()` は `printf` で primary console(UART0)にしか出ず
USB Serial/JTAG のモニタに現れない(Phase 12 の教訓)。そのため
`heap_caps_walk(MALLOC_CAP_INTERNAL, ...)` で領域ごとに集計し直し `ESP_LOG` で出す
実装にした。

### O-1. 旧 config 名(`CONFIG_ESP32S3_SPIRAM_SUPPORT`)が立つか — **立つ**

| 確認項目 | 結果 |
|---|---|
| WAMR のバージョン | **2.4.0**(`core/version.h`。指示書 F1〜F7 が前提とした版と一致) |
| `core/shared/platform/esp-idf/shared_platform.cmake` に F3 の記述 | **ある**(`if(CONFIG_ESP32S3_SPIRAM_SUPPORT) add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1) endif()`) |
| IDF 5.5 のリネーム定義 | `/opt/esp-idf/components/esp_hw_support/sdkconfig.rename.esp32s3` に `CONFIG_ESP32S3_SPIRAM_SUPPORT → CONFIG_SPIRAM` |
| kconfgen が旧名を出力するか | **する**。PSRAM 無効の現行ビルドでも `build/config/sdkconfig.cmake:1882` に `set(CONFIG_ESP32S3_SPIRAM_SUPPORT "")` があり、旧名は「値付きで必ず出力される」扱い(周辺も `CONFIG_ESP32S3_BROWNOUT_DET_LVL` 等の旧名が並ぶ)。したがって `CONFIG_SPIRAM=y` にすれば旧名も `"y"` になる |
| CMake の真偽 | `"y"` は CMake の偽定数(`OFF/0/NO/FALSE/N/IGNORE/NOTFOUND/""`)に含まれないので `if()` は真になる |

→ **A 案の前提(F3 の経路)は机上では成立する。** 実測は C1/C2 で確認する。

`WASM_MEM_DUAL_BUS_MIRROR` の他の使用箇所は AOT 関連
(`aot_loader.c` / `aot_reloc_xtensa.c`)と `platform_api_vmcore.h` の宣言のみで、
`CONFIG_WAMR_ENABLE_AOT=n` の本構成では `espidf_memmap.c` だけが影響を受ける。

### O-2. linear memory の実アドレス

F1・F2 はソースで再確認済み:

- `wasm_memory.c: wasm_allocate_linear_memory()` は `WASM_MEM_ALLOC_WITH_USAGE`
  (`core/config.h` の既定 **0**)が無効なら `wasm_mmap_linear_memory()` → `os_mmap()`。
  プール(`Alloc_With_Pool` の 48KB)は使わない。
- `espidf_memmap.c: os_mmap()` は非実行領域に対し
  `heap_caps_malloc(size + 4 + sizeof(uintptr_t), MALLOC_CAP_8BIT)`。
  `WASM_MEM_DUAL_BUS_MIRROR != 0` なら `MALLOC_CAP_SPIRAM` に切り替わる。

実測(instantiate 直後にログ):

| 条件 | PSRAM | SD 経路 | memory_data アドレス | サイズ |
|---|---|---|---|---|
| C0 | 無効(現行 main) | SDMMC 20.00MHz | **0x3fcf02e8**(internal DRAM) | 16,496〜17,696(アプリ別) |

C0 のアプリ別サイズ:

| アプリ | `__heap_base` | memory_data サイズ | アドレス |
|---|---|---|---|
| touch_demo | 8,304 | 16,496 | 0x3fcf02e8 |
| mp3player | 9,504 | 17,696 | 0x3fcf02e8 |
| metronome | 8,448 | 16,640 | 0x3fcf02e8 |
| midi_loopback | 8,768 | 16,960 | 0x3fcf02e8 |
| seq_smoke | 8,560 | 16,752 | 0x3fcf02e8 |

### O-3. internal ヒープの領域別内訳 — **31,744 は「独立した 32KB 領域」で確定**

C0(touch_demo 実行中)の `heap_caps_walk(MALLOC_CAP_INTERNAL, ...)` 集計:

| region | 範囲 | サイズ | free | largest free | blocks |
|---|---|---|---|---|---|
| 0 | 0x600fe06c–0x600fffe8 | 8,060 | 7,672 | 7,672 | 1 |
| 1 | 0x3fce9710–0x3fceee34 | 22,308 | 0 | 0 | 97 |
| 2 | **0x3fcf0000–0x3fcf8000** | **32,768** | 14,612 | 14,612 | 2 |
| 3 | 0x3fcc1800–0x3fce9710 | 163,600 | 7,600 | **6,944** | 122 |

- **linear memory(0x3fcf02e8)は region 2 に入っている。** 16〜18KB の連続確保が
  できる領域はここしかない(region 3 は 163KB あるが 122 ブロックに断片化していて
  最大連続 6,944 B)。
- ランチャー待機時に region 2 は空なので largest free block = 32,768 − 1,024(管理領域)
  = **31,744**。Phase 12 の「独立した 32KB DRAM 領域が与える構造的上限」という説明は
  **正しい**。ただし補足が要る: **region 3 が断片化しているため、内部 RAM の空きが
  増えても最大連続ブロックは伸びない**という二重の理由である。
- P10-4 の `heap_init` 内訳(163KiB + 21KiB + 32KiB DRAM + 7KiB RTCRAM)と一致する。

### O-4. `.wasm` バッファ・LVGL 描画バッファ・L0/L1 の確保箇所の棚卸し

| 対象 | 確保箇所 | 種別 / caps | サイズ | PSRAM へ流出しうるか |
|---|---|---|---|---|
| `.wasm` バッファ | `wasm_runtime.cpp: read_wasm_file()` の `malloc(size)` | 一般ヒープ(`MALLOC_CAP_DEFAULT` 相当) | 現状の最大は midi_loopback の **5,947 B**(上限チェックは 512KB) | **する**(`SPIRAM_USE_MALLOC` なら自動、`CAPS_ALLOC` なら明示変更が要る) |
| LVGL 描画バッファ | `esp_lvgl_port` の `lvgl_port_add_disp()` → `heap_caps_malloc(buffer_size * 2, buff_caps)` ×2(double buffer) | `disp_cfg.flags.buff_dma = 0` / `buff_spiram = 0` なので `buff_caps = MALLOC_CAP_DEFAULT` | `240 × 40 × 2 B = 19,200 B` ×2 = **38,400 B** | **する**(`MALLOC_CAP_DEFAULT` 経由) |
| LVGL タスクスタック | `lvgl_port_cfg.task_stack_caps = MALLOC_CAP_INTERNAL \| MALLOC_CAP_DEFAULT` | **internal 明示** | 8,192 B | しない |
| WAMR プール | `wasm_runtime.cpp` の `static uint8_t s_wamr_heap[48*1024]` | **静的 BSS** | 49,152 B | しない |
| L0 キュー | `shared/seq_core.c` の `static hostapi_seq_event_t s_queue[256]` | **静的 BSS** | 4,096 B | しない |
| テンポマップ / 拍子マップ | `shared/seq_core.c` の `static TempoEntry s_tempo[32]` / `s_meter[32]` | **静的 BSS** | 小 | しない |
| L1 の状態(`s_seg_*` 等) | `shared/seq_core.c` の static 変数 | **静的 BSS** | 小 | しない |
| クリックタスク | `audio.cpp` の `xTaskCreateStatic` + 静的スタック | **静的 BSS** | — | しない |
| シリアルコンソールタスク | `serial_cmd.cpp` の `xTaskCreateStatic` + 静的スタック | **静的 BSS** | — | しない |
| FreeRTOS タスクスタック(`xTaskCreate` 組) | power_key / midi_rx / wasm_boot / wasm アプリ pthread | IDF はタスクスタックを既定で internal に置く(`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` 未設定) | — | しない |
| DMA ディスクリプタ / I2S バッファ | IDF ドライバ内部(`MALLOC_CAP_DMA`) | DMA 可能 = internal | — | しない |

**§9 の方針は暗黙に崩れない。** L0 キュー・テンポマップ・L1 の状態はすべて
`shared/seq_core.c` の **静的 BSS** であり、`CONFIG_SPIRAM_USE_MALLOC` を選んでも
PSRAM へは移らない(静的データはリンカが internal DRAM に置く)。
流出しうるのは `.wasm` バッファと LVGL 描画バッファの 2 つだけで、
どちらも §9 が internal 固定を要求している対象ではない。

### O-5. アプリ側のメモリ宣言 — **指示書 F4 は本構成には当てはまらない**

`.wasm` の memory セクションはどれも **initial 1 page(64KB)/ max 指定なし**。
にもかかわらず実測の linear memory は 16〜18KB である。理由は WAMR の
**`WASM_ENABLE_SHRUNK_MEMORY`(`core/config.h` の既定 1)**:

`wasm_loader.c`(2.4.0, L6498〜6570)は `module->possible_memory_grow` が偽のとき、

1. `num_bytes_per_page = align8(__heap_base)` / `init_page_count = 1` に**縮める**
   (宣言された 64KB は捨てられる)、
2. 続けて「one big page」化で `init_page_count = max_page_count = 1` にする。

その結果 `memory_instantiate()` の最初の分岐
(`init_page_count == max_page_count && init_page_count == 1`)に入り、
`num_bytes_per_page += heap_size` で **app heap(instantiate の第 3 引数 8KB)を
末尾に足す**。つまり:

> **linear memory サイズ = align8(`__heap_base`) + 8,192**

C0 の実測 5 本すべてがこの式に 1 バイトの狂いもなく一致した(上表)。

したがって **`--initial-memory` / `--max-memory` を増やしても linear memory は
増えない**(SHRUNK が上書きするため)。実際に増やせる操作は:

| 操作 | 効き方 | 指定箇所 |
|---|---|---|
| `-C link-arg=-zstack-size=N` を増やす | `__stack_pointer` が上がり、データ配置がその後ろへ動くので `__heap_base` が増える | `wasm-apps/<app>/.cargo/config.toml`(現行はすべて **8192**) |
| アプリに大きな `static` を足す | `__data_end` → `__heap_base` が増える | アプリの Rust ソース |
| instantiate の `heap_size` を増やす | 末尾の app heap が増える | `wasm_runtime.cpp` の `wasm_runtime_instantiate(module, 8*1024, 8*1024, ...)`(native 側・全アプリ共通) |

**T-3(初期ページ数を引き上げた .wasm が起動するか)は、`-zstack-size` を上げる形で
実施するのが正しい。** 「初期ページ数」を直接いじっても SHRUNK に潰される。

### 実測表(O-2 / O-3 の 3 条件)

| 条件 | PSRAM | SD 経路 | `CONFIG_ESP32S3_SPIRAM_SUPPORT` | `WASM_MEM_DUAL_BUS_MIRROR` | **memory_data アドレス** | サイズ |
|---|---|---|---|---|---|---|
| C0 | 無効(現行 main) | SDMMC 20.00MHz | `""` | 未定義 | **0x3fcf02e8**(internal DRAM) | 16,496〜17,696 |
| C1 | 有効 + `SPIRAM_USE_CAPS_ALLOC` | E5 迂回(SDSPI 11.43MHz) | **`"y"`** | **`=1`(37 コンパイル行)** | **0x3c101820**(PSRAM) | 16,496〜17,696 |
| C2 | 有効 + `SPIRAM_USE_MALLOC` | E5 迂回(SDSPI 11.43MHz) | **`"y"`** | **`=1`** | **0x3c101820**(PSRAM) | 16,496〜17,696 |

いずれの条件でもサイズは C0 と 1 バイトも変わらない(= linear memory の大きさは
PSRAM の有無と無関係で、O-5 の式だけで決まる)。

C1 のビルド時確認:

```
set(CONFIG_SPIRAM_USE_CAPS_ALLOC "y")
set(CONFIG_ESP32S3_SPIRAM_SUPPORT "y")      ← 旧名が値付きで立つ(O-1 の予測どおり)
===DUALBUS=== WASM_MEM_DUAL_BUS_MIRROR=1    ← compile_commands.json に 37 件
```

C1 の INTERNAL ヒープ(touch_demo 実行中、C0 と同じ時点):

| region | 範囲 | サイズ | free | largest free | blocks | C0 との差 |
|---|---|---|---|---|---|---|
| 0 | 0x600fe06c–0x600fffe8 | 8,060 | 7,672 | 7,672 | 1 | 同一 |
| 1 | 0x3fce9710–0x3fceee34 | 22,308 | 0 | 0 | 78 | 同一(ブロック数のみ減) |
| 2 | 0x3fcf0000–0x3fcf8000 | 32,768 | **32,024** | **32,024** | 1 | **linear memory が抜けて空になった** |
| 3 | 0x3fcc3530–0x3fce9710 | 156,128 | 48,744 | **42,280** | 116 | PSRAM 管理領域で先頭が 7,472 B 上がり、LVGL バッファ(38,400 B)が抜けて空きが増えた |

- **`INTERNAL largest free block` は 31,744 → 40,960 に増えた**(region 3 の 42,280 が
  最大になった。C0 では region 2 の 31,744)。
- `wasm_buf`(`.wasm` の malloc)は `0x3fcd84a8` で **internal のまま**
  (`CAPS_ALLOC` では `malloc()` が PSRAM を返さないため)。
- 一方 LVGL 描画バッファは PSRAM へ移った。`CAPS_ALLOC` でも
  **`MALLOC_CAP_DEFAULT` には PSRAM が含まれる**ことの実証(指示書 F8 の記述どおり)。
- 全 5 アプリが起動し、許容外の WARN/ERROR は **0 件**。

**`device-regress.sh` の判定は PSRAM 有効構成では機能しない**(既知・要対処):
`esp_get_free_heap_size()` と `heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)` が
PSRAM を含むため、free heap 8,407,092 / largest 8,257,536 になり
`EXPECT_LARGEST=31744` と一致せず全アプリ FAIL 表示になる(Phase 12 E7 と同じ現象)。
**ログ上の実質は PASS**(WARN/ERROR 0 件、全アプリ起動・停止)。
→ ステップ 2 に入る前に、スクリプト側の判定を INTERNAL 基準に変える提案を出す(ゲート 4)。

補足(O-4 の裏取り): `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` は IDF 5.5 では
`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` へ改名された旧名で、既定 y。
Kconfig の help に **「`xTaskCreate` が確保するタスクスタックは既定で internal RAM から
取られる」**と明記されており、この設定は `xTaskCreateStatic` に外部メモリのバッファを
渡すことを許すだけである。本プロジェクトの静的スタックはすべて BSS(internal)なので
影響しない。

### C2(`SPIRAM_USE_MALLOC`)の内訳と C1 との比較

C2 の INTERNAL ヒープ(touch_demo 実行中)は **5 領域**になる:

| region | 範囲 | サイズ | free | largest free | blocks |
|---|---|---|---|---|---|
| 0 | **0x3fcc88e8–0x3fcd08e7** | 32,767 | 32,020 | 32,020 | 1 |
| 1 | 0x600fe06c–0x600fffe8 | 8,060 | 7,672 | 7,672 | 1 |
| 2 | 0x3fce9710–0x3fceee34 | 22,308 | 0 | 0 | 83 |
| 3 | 0x3fcf0000–0x3fcf8000 | 32,768 | 32,024 | 32,024 | 1 |
| 4 | 0x3fcc3530–0x3fce9710 | 156,128 | 15,936 | **9,472** | 113 |

region 0 は `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`(既定 **32,768**)が
大きな internal 領域から切り出した予約プールで、その分 region 4 が痩せる。

3 条件の比較(touch_demo 実行中、INTERNAL のみ):

| 条件 | INTERNAL free | INTERNAL largest | `.wasm` バッファ | LVGL 描画バッファ | memory_data |
|---|---|---|---|---|---|
| C0(PSRAM 無効) | 29,920 | **14,336**(アイドル時 31,744) | internal 0x3fcdc3a0 | internal | **internal** |
| C1(`CAPS_ALLOC`) | 88,480 | **40,960** | internal 0x3fcd84a8 | **PSRAM へ移動** | **PSRAM** |
| C2(`USE_MALLOC`) | 87,703 | **31,744** | internal 0x3fce04d0(1,549 B < `MALLOC_ALWAYSINTERNAL` 16,384 なので internal) | **PSRAM へ移動** | **PSRAM** |

- **C1 のほうが largest free block が大きい**(40,960 対 31,744)。C2 は予約プールの
  切り出しで大領域が断片化するため。
- C1 は `malloc()` が PSRAM を返さないので、**ドライバ内部の一般 `malloc` が
  意図せず PSRAM へ流れるリスクがない**。C2 は 16KB 超の `malloc` が自動で PSRAM に行く。
- 許容外の WARN/ERROR は C0 / C1 / C2 いずれも **0 件**、全 5 アプリが起動・停止した。

### 想定外の副作用(スコープとの矛盾。判断を仰ぐ)

**PSRAM を有効にすると LVGL 描画バッファ(38,400 B)が自動的に PSRAM へ移る。**
`esp_lvgl_port` は `flags.buff_dma = 0` / `buff_spiram = 0` のとき
`buff_caps = MALLOC_CAP_DEFAULT` を使い、`MALLOC_CAP_DEFAULT` には PSRAM が含まれる
(`CAPS_ALLOC` でも `USE_MALLOC` でも同じ)。指示書は「LVGL 描画バッファの PSRAM 移動」を
**スコープ外**としているので、意図せずスコープに入ってしまう。

選択肢:

| 案 | 実装 | 効果 |
|---|---|---|
| L-a: 受け入れる | 変更なし | internal を 38,400 B 節約。C1 の largest 40,960 はこの効果込み。C0〜C2 の実測で描画の異常・WARN は無かったが、**画面の目視確認は未実施** |
| L-b: internal に固定する | `display.cpp` に `disp_cfg.flags.buff_dma = true;` を 1 行(`MALLOC_CAP_DMA` が付き PSRAM 対象外になる) | 現行の挙動を厳密に維持。ただし internal largest は C0 相当まで戻る見込み |

### ステップ 0 の結論 — **A 案は成立する(yes)**

> **Q: 「A 案(PSRAM 有効化のみで linear memory が PSRAM に行く)が成立するか」**
> **A: yes。** `CONFIG_SPIRAM=y` にするだけで、
> IDF のリネーム機構が `CONFIG_ESP32S3_SPIRAM_SUPPORT="y"` を立て →
> WAMR の `shared_platform.cmake` が `-DWASM_MEM_DUAL_BUS_MIRROR=1` を付け →
> `espidf_memmap.c: os_mmap()` の確保フラグが `MALLOC_CAP_SPIRAM` になり →
> **linear memory は PSRAM(0x3c101820)から取られる**。
> `CAPS_ALLOC` / `USE_MALLOC` のどちらでも成立し、`managed_components/` の
> 書き換えは不要(ゲート 5 に抵触しない)。

**Phase 12 の「効果なし」判定は、指標の取り方の問題だった。** largest free block が
31,744 のまま動かなかったのは事実だが、それは WASM の可否を表す指標ではない。
Phase 12 は「largest が増えない=linear memory の逼迫は緩和されない」と結論したが、
実際には **linear memory はその領域から出て行っていた**(当時は memory_data の
アドレスを見ていなかったため気づけなかった)。

**指示書 F4 は本構成には当てはまらない**(O-5)。`--initial-memory` を上げても
`WASM_ENABLE_SHRUNK_MEMORY` に潰される。実際に効くのは `-zstack-size` である。

---

## ステップ 1: 設計メモ

`docs/design/phase15-psram.md` に記載(承認済み)。決定の要旨:

| 項目 | 決定 |
|---|---|
| 採用案 | **A 案 + `CONFIG_SPIRAM_USE_CAPS_ALLOC`**。`sdkconfig.defaults` のみの変更 |
| B 案(`.wasm` バッファの PSRAM 化) | **本フェーズでは採らない**(現状最大 5,947 B で効果がない)。再検討トリガ = いずれかの `.wasm` が 16KB を超えた時点 |
| LVGL 描画バッファ | **L-a(PSRAM を受け入れる)を暫定採用**。ただしフォールバック条件を明記 |
| `CONFIG_SPIRAM_XIP_FROM_PSRAM` | **採らない**(判断のみ記録)。`.text` を PSRAM へ移すとコード実行速度が変わり、T-1 が落ちたときの原因切り分けができなくなる |
| 回帰の指標 | ファーム側を **internal 基準**に改め、PSRAM を別系統で足して **4 値**にする |
| T-3 の引き上げ | `touch_demo` の `-zstack-size` を 8,192 → 65,536(`--initial-memory` ではない) |

---

## ステップ 2: 実装(2026-09-06)

### 2-1. 変更一覧

| ファイル | 変更 |
|---|---|
| `src/sdkconfig.defaults` | PSRAM 設定 4 行(`CONFIG_SPIRAM` / `MODE_OCT` / `SPEED_80M` / `USE_CAPS_ALLOC`)。設定値ごとに根拠コメント |
| `src/main/Kconfig.projbuild` | `MIDIBOX_SD_SKIP_SDMMC_PROBE` を追加(`default y if SPIRAM`)。Phase 12 E5 の迂回を一時マクロから Kconfig へ格上げした |
| `src/components/storage/sdcard.cpp` | SDMMC プローブのガードを `PHASE15_NO_SDMMC_PROBE` から `CONFIG_MIDIBOX_SD_SKIP_SDMMC_PROBE` へ |
| `src/components/wasm_runtime/wasm_runtime.cpp` | **常設**: `app: linear memory <addr> size <n> (PSRAM \| internal DRAM)` と 4 値の heap ログ。`app: stopped` を internal 基準へ + 4 値行を追加 |
| `src/components/display/display.cpp` | `io_config.flags.psram_dma_direct = 1`。**常設**: 描画バッファのアドレスと確保前後の internal / PSRAM 差分。LVGL フラッシュ時間の統計(T-2) |
| `src/components/display/CMakeLists.txt` | `REQUIRES` に `esp_timer` |
| `src/components/wasm_runtime/launcher.cpp` / `src/main/app_main.cpp` | heap ログを `esp_get_free_heap_size()` / `MALLOC_CAP_DEFAULT` から internal 基準へ |
| `src/main/serial_cmd.cpp` | `heap` コマンドに PSRAM 側の 3 値を追加 |
| `src/CMakeLists.txt` | ステップ 0 の一時定義を削除(元の内容に戻した) |

**Host API / ABI の変更はない**(ゲート 3)。`managed_components/` にも触っていない(ゲート 5)。

### 2-2. `app: stopped` の書式について

`device-regress.sh` は `free heap` / `(at start` / `largest block` の語でパースしている。
**1 行目はその書式のまま残し、値の意味だけ internal 基準に改めた。**
これで 4b(スクリプト側の変更、ゲート 4)が承認待ちの間もスクリプトは動き続け、
しかも読み取る値は正しい internal の値になる。2 行目が 4 値の正本である。

```
app: stopped (ok), free heap <free_int> (at start <free_int>), largest block <largest_int>
app: stopped free_int=… largest_int=… free_psram=… largest_psram=… [start free_int=… free_psram=…]
```

2 行目に `(at start <数字>` / `free heap ` / `largest block ` のいずれの語も含めていないのは、
スクリプトの `sed` が貪欲マッチで**最後の出現**を拾うため、誤って 2 行目を読まないようにする配慮である。

### 2-3. T-3 用 `.wasm` の事前確認(ホスト側)

`touch_demo` の `-zstack-size` を 8,192 → 65,536 にして `cargo build --release`:

| 項目 | 値 |
|---|---|
| memory セクションの宣言 | initial **2 pages**(= 131,072 B。SHRUNK に潰されるので無関係) |
| `__stack_pointer` の初期値 | 65,536 |
| `__heap_base` | **65,648** |
| 予想 linear memory | align8(65,648) + 8,192 = **73,840 B** |

**73,840 B は C1 で実測した internal largest 40,960 を大きく超える。**
internal からは原理的に取れないので、起動すれば PSRAM で動いていることの決定的な証明になる。
(この時点では `.cargo/config.toml` は 8,192 に戻してある。T-3 の実施時に適用する。)

### 2-4. 途中で踏んだこと

- `display.cpp` に `esp_timer.h` を足したところ、コンポーネントの `REQUIRES` に
  `esp_timer` が無く `fatal error: esp_timer.h: No such file or directory`。追加して解消。
- ビルド完了待ちで `grep -q BUILD_DONE` を使ったところ、**スクロールバックに残る
  前回(失敗した)ビルドの `BUILD_DONE` に誤マッチ**した。`hpane.sh run` が付ける
  今回固有のセンチネル(`HPANE_<epoch>_<pid>_EXIT`)で待ち直した。
  `docs/workflow.md` §1-2 の不変条件がそのまま当てはまる事例。

### 2-5. 実機確認(`captures/phase15-step2/`)

```
lvgl draw buf: active 0x3c0f0998 (PSRAM) size 19200, cfg 19200 B x2, internal 60 B, psram 38920 B
app: linear memory 0x3c101820 size 16496 (PSRAM), wasm buf 0x3fcd475c size 1549
app: stopped free_int=105880 largest_int=57344 free_psram=8316904 largest_psram=8257536
                                     [start free_int=105880 free_psram=8316904]
```

| 指標(アプリ実行中) | C0(PSRAM 無効) | C1(ステップ 0) | **ステップ 2** |
|---|---|---|---|
| linear memory の置き場 | internal | PSRAM | **PSRAM** |
| `free_int` | 29,920 | 88,480 | **105,880** |
| `largest_int` | 14,336 | 40,960 | **57,344** |
| `free_psram` | 0 | 8,300,004 | 8,316,904 |
| 許容外 WARN/ERROR | 0 | 0 | **0** |

- **5 アプリすべてで `free_int` / `free_psram` が開始値と終了値で完全一致**(差分 0)。
  新設した PSRAM 側のリーク監視が初回から機能している。
- LVGL の描画バッファは 2 面ぶん 38,920 B が PSRAM から出て、internal 消費は **60 B**。

### 2-6. T-2 の一部: `psram_dma_direct` の 0 / 1 比較 — **1 を採用**

同一コードで `io_config.flags.psram_dma_direct` だけを変えた 2 ビルドを実機で比較した
(`captures/phase15-step2/` = 1、`captures/phase15-t2-nodirect/` = 0)。

| 指標 | `psram_dma_direct = 0` | **`= 1`(採用)** | 差 |
|---|---|---|---|
| `lvgl_port_add_disp` の internal 消費 | **19,520 B** | **60 B** | −19,460 |
| `free_int`(アプリ実行中) | 86,420 | **105,880** | **+19,460** |
| `largest_int` | 36,864 | **57,344** | **+20,480** |
| free_int の開始→終了差分 | **+6,656 〜 +17,600(アプリ毎にばらつく)** | **すべて +0** | — |
| LVGL flush avg | **663 µs** | 823 µs | +160 |
| LVGL flush p95 / p99 | 869 / 1,060 µs | 1,104 / 1,567 µs | +235 / +507 |
| LVGL flush **max** | **8,814 µs** | **1,701 µs** | **−7,113** |

**設計メモ §3-2 で予測した spi_master の internal バウンスが実在した。**
`psram_dma_direct = 0` では色バッファが PSRAM にあるのに `SPI_TRANS_DMA_USE_PSRAM` が
立たないため、`setup_dma_priv_buffer()` が転送のたびに
`heap_caps_aligned_alloc(..., MALLOC_CAP_INTERNAL)` + `memcpy` を行う。
フラッシュ 1 回ぶん = 19,200 B で、実測の −19,460 B / −20,480 B とよく一致する。

**重要な副産物: ステップ 0 の C1/C2 で free heap 差分が +6,656 → +17,600 と
単調に増えて見えた現象の正体は、この一時バッファだった。**
測定時点でまだ転送中のバウンスバッファが未解放だっただけで、PSRAM 側のノイズでも
リークでもない。`psram_dma_direct = 1` にすると差分は全アプリ 0 になる。

**1 を採る判断**: 平均フラッシュ時間は 160 µs 悪化するが、100ms の tick 周期に対して
0.16% であり無視できる。対して得られるものが大きい:

1. internal を 19,460 B 取り戻す(PSRAM 化の目的そのもの)。
2. **最悪値が 8,814 µs → 1,701 µs へ 5 分の 1 になる。** 平均より裾が重要。
3. **ヒープ差分が決定的に 0 になる。** 0 のままだと差分がアプリ毎にばらつき、
   せっかく作ったリーク検出が機能しない。

### 2-7. 回帰スクリプトの改訂(4a / 4b)

**4a(ファーム側・ゲート不要)** はステップ 2 本体に含めた(§2-1、§2-2)。

**4b(`scripts/` の変更・ゲート 4。ユーザー承認済み)**:

`device-regress.conf`:

| 変更 | 内容 |
|---|---|
| 削除 | `EXPECT_LARGEST=31744`(固定値一致) |
| 追加 | `MIN_FREE_INT=80000` / `MIN_LARGEST_INT=32768` / `MIN_FREE_PSRAM=8000000`(下限しきい値) |
| 追加 | `EXPECT_DELTA_PSRAM`(PSRAM 側のリーク検出。既定 0) |

`device-regress.sh`:

- 4 値行 `app: stopped free_int=… largest_int=… free_psram=… largest_psram= … [start …]` を
  読むようにした。
- **判定を 2 つに分離した**: リーク検出は差分の厳密一致(int / psram の両系統)、
  余裕の監視は下限しきい値。**この 2 つを混ぜていたのが Phase 14 までの設計の弱点。**
- 表を 8 列に拡張(`開始 free_int` / `終了 free_int` / `int 差分` / `largest_int` /
  `psram 差分` / `largest_psram`)。

**パースの注意点(実装中に踏んだ)**: `free_int` / `free_psram` は 4 値行の前半と
末尾の `[start …]` の両方に出る。`sed` の `.*` は貪欲なので、行を
`[start` で切り分けてから読まないと**最後の出現 = 開始値**を拾ってしまい、
差分が常に 0 になって**リーク検出が黙って無効化される**。切り分けてから読む実装にした。

**4c(同一アプリ N 回反復での非減少判定)は次フェーズへ繰り越す**(ユーザー承認済み)。
`docs/status.md` に「PSRAM のリーク監視は 1 回の起動→停止の差分までで、
反復時の非減少は未実装」と明記すること。

### 2-8. 副次的な発見: `CAPS_ALLOC` は `heap_caps_malloc(..., MALLOC_CAP_DEFAULT)` を止めない

`psram_dma_direct` の調査中に、**`malloc()` と
`heap_caps_malloc(size, MALLOC_CAP_DEFAULT)` が `CAPS_ALLOC` 下で違う挙動をする**ことが
実測とソースの両方で確認できた。

実測(ステップ 2、`CAPS_ALLOC`):

| 確保 | 呼び出し | 結果 |
|---|---|---|
| `.wasm` バッファ | `malloc(size)` | **internal**(0x3fcd475c) |
| LVGL 描画バッファ | `heap_caps_malloc(size, MALLOC_CAP_DEFAULT)` | **PSRAM**(0x3c0f0998) |

ソース(`components/heap/heap_caps.c`):

```c
HEAP_IRAM_ATTR void *heap_caps_malloc_default( size_t size )   // malloc() の実体
{
    if (malloc_alwaysinternal_limit == MALLOC_DISABLE_EXTERNAL_ALLOCS) {
        return heap_caps_malloc( size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    }
    ...
```

`CAPS_ALLOC` では `malloc_alwaysinternal_limit == MALLOC_DISABLE_EXTERNAL_ALLOCS` なので、
`malloc()` は `MALLOC_CAP_INTERNAL` を**足して**呼び直される。
一方、呼び出し側が `heap_caps_malloc(..., MALLOC_CAP_DEFAULT)` を**直接**呼ぶと
このガードを通らず、PSRAM は `MALLOC_CAP_DEFAULT` を持っているので PSRAM から取れてしまう。

**これは Phase 12 の E3 の結論を弱める。** E3 は
「`SPIRAM_USE_CAPS_ALLOC` にしても同じ失敗 → PSRAM 由来バッファが DMA 経路に渡る筋は消えた」
と判定したが、**`CAPS_ALLOC` は `heap_caps_malloc(..., MALLOC_CAP_DEFAULT)` を止めない**ので、
SD ドライバ内部がその形で確保していれば PSRAM のバッファが渡りうる。
**H5(PSRAM 由来バッファが DMA 経路へ)はまだ生きている仮説である。**
ステップ 4 でこれを扱う。

---

## ステップ 3: 計測

### T-3: 初期ページ数を引き上げた `.wasm` の起動 — **PASS**(本フェーズの本当の合否)

`wasm-apps/touch_demo/.cargo/config.toml` の `-zstack-size` を 8,192 → **65,536** にして
`cargo build --release` → ファーム再ビルド → フラッシュ。
生データ: `captures/phase15-t3-64k/`。

```
app: linear memory 0x3c101820 size 73840 (PSRAM), wasm buf 0x3fcd475c size 1549
```

| 項目 | 値 |
|---|---|
| `__heap_base` | 65,648 |
| 予想 linear memory(align8(`__heap_base`) + 8,192) | 73,840 |
| **実測 linear memory** | **73,840**(予測と完全一致) |
| 置き場 | **PSRAM**(0x3c101820) |
| そのときの internal largest | **57,344** |

**73,840 B は internal の最大連続ブロック 57,344 を超えており、internal からは原理的に
取れない。** それが起動したことが、linear memory が PSRAM で動いていることの決定的な証拠である。
C0(PSRAM 無効)の largest 31,744 に対しては 2.3 倍にあたる。

同じビルドで `device-regress.sh` を通し、**5 アプリすべて PASS**
(int 差分 +0 / psram 差分 +0 / largest_int 57,344 / 許容外 WARN・ERROR 0 件)。
**引き上げたアプリを混ぜても既存 4 本の回帰は 1 バイトも動かなかった。**

#### T-3 の上限探索

`-zstack-size` を上げて「どこまでで失敗するか」を見た。生データ:
`captures/phase15-t3-8m/` / `captures/phase15-t3-limit/`。

| `-zstack-size` | `__heap_base` | 予想 linear memory | 実機 | 備考 |
|---|---|---|---|---|
| 8,192(基準) | 8,304 | 16,496 | **起動** | C0 と同じ |
| 65,536 | 65,648 | **73,840** | **起動**(実測 73,840) | internal largest 57,344 を超える |
| 8,248,832 | 8,248,944 | **8,257,136** | **起動**(実測 8,257,136) | `largest_psram` 8,257,536 の 400 B 下。**基準の 500 倍** |
| 8,388,608(8MB) | 8,388,720 | **8,396,912** | **失敗** | `largest_psram` を 139,376 B 超過 |

8MB のときの失敗ログ:

```
MBCMD: run ok /sdcard/apps/touch_demo.wasm
instantiate: WASM module instantiate failed: allocate linear memory failed
```

**失敗は `os_mmap` の `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` が返らなかったことによる
確保失敗であり、`memory_instantiate` のページ数判定
(`default_max_page` 等)には当たっていない。** 同じ実行で他の 4 アプリは
すべて正常に起動・停止しており(int/psram 差分 0)、**失敗は当該アプリに閉じている**
(ランチャーはエラー表示に落ちるだけでリブートしない)。

したがって **上限は「PSRAM の最大連続ブロック(実測 8,257,536 B)」で決まる**。
境界の両側を実機で押さえた:

- **8,257,136 B(largest_psram の 400 B 下)→ 起動する**
- **8,396,912 B(largest_psram を 139,376 B 超過)→ `allocate linear memory failed`**

基準の 16,496 B に対して **約 500 倍**。PSRAM 化前は internal の 31,744 B が天井だったので、
**WASM アプリが使えるメモリの上限は internal ヒープの最大連続ブロックから完全に切り離された**
(本フェーズの目的そのもの)。

補足: `-zstack-size` は **16 バイト境界**でなければ `rust-lld` が
`stack size must be 16-byte aligned` で失敗する(8,249,000 で踏んだ)。

### T-4: 20 回連続再起動 — **未完(1/20 実施で中断)**

指示書どおり `device-regress.sh` を 20 周させる形で開始したが、**セッションのレート制限
(95% 到達)によりユーザー判断で 1 周目完了・2 周目の途中で中断した。**

| 周 | 結果 |
|---|---|
| run1 | **PASS**(5 アプリ、int 差分 +0 / psram 差分 +0 / largest_int 57,344 / 許容外 WARN・ERROR 0 件) |
| run2 | 中断(`captures/phase15-t4/run2/monitor.log` のみ残存) |
| run3〜20 | **未実施** |

生データ: `captures/phase15-t4/run1/`。**再開時は run1 から測り直すこと**
(途中の 1 周だけを積み上げても連続性の証明にならない)。

### T-1 / T-2 — **未実施**

- **T-1(metronome の MIDI クロック)**: 実機 MIDI OUT → UM-ONE → PC の物理配線と、
  metronome の START/STOP のタップが要るため未実施。UM-ONE がホストに見えていることは
  確認済み(`aconnect -l` に `client 20: 'UM-ONE'`)。
- **T-2 の `app_tick` 統計**: 1,000 サンプル = 100 秒かかるため、回帰の保持時間
  (6〜20 秒)では到達しない。**T-1 の 5.5 分測定で同時に取得する**のが効率的。
  T-2 のうち **LVGL フラッシュ時間は取得済み**(§2-6)。

### ステップ 3 のカメラ目視確認 — **未実施**

ティアリング・描画欠け・色化けは WARN/ERROR に出ないので、`docs/workflow.md` §3.3 の
人間操作 + カメラで見る必要がある。**LVGL 描画バッファを PSRAM に置いた(L-a)ので、
これは省略できない。**

---

## 中断時点のまとめ(2026-09-06)

### 達成したこと

| 指示書の完了条件 | 状態 |
|---|---|
| 1. ステップ 0 の表が揃い、A〜D のどれを採るかが根拠付きで決まっている | **達成**(A 案 + `CAPS_ALLOC`) |
| 2. 初期ページ数を引き上げた `.wasm` が実機で起動する(T-3) | **達成**(73,840 B / 8,257,136 B。上限機構も確定) |
| 3. metronome が Phase 13 と同じ絶対値目標を満たす(T-1) | **未実施** |
| 4. 20 回連続再起動(T-4)、全アプリ回帰が合格 | **回帰は合格。20 回連続は 1/20 で中断** |
| 5. SDMMC と PSRAM の共存可否の結論 | **未実施**(ステップ 4。H5 が生きている点だけ記録) |
| 6. `docs/architecture.md` §9・§11-2 と `docs/lessons.md` の更新 | **§9 と lessons.md は完了。§11-2 は T-1 の結果待ち** |

### 現在の main の構成

**PSRAM 有効のまま残している。** ただし **T-1 が未実施**なので、指示書の
「T-1 が不合格なら PSRAM 無効に戻す」という条件はまだ検証されていない。
**次のセッションで最初に T-1 を実施すること。**

### 次のセッションでやること(順序)

1. **T-1**(物理操作あり)。`seq_smoke` で計測系の妥当性確認 → metronome を
   5.5 分。`./scripts/midi-clock-probe.sh --task phase15 --label A1 --duration 330 --bpm 120`。
   同じ測定で **T-2 の `app_tick` 統計**も取れる(1,000 サンプル = 100 秒)。
2. **T-4 を run1 からやり直す**(20 周、約 35 分の無人実行)。
3. **ステップ 3 のカメラ目視確認**(L-a を採ったので必須)。
4. **ステップ 4**(SDMMC と PSRAM の共存)。**H1 は否定済み**(SDMMC は
   CLK=14 / CMD=17 / D0=16、octal PSRAM は GPIO 33〜37 で重ならない。Phase 12 E1 と一致)。
   **H5 は §2-8 のとおり生き返っているので最初に当たる。**
5. **ステップ 5**。`docs/architecture.md` §11-2 の改訂、`docs/status.md` の更新
   (**4c 未実装 = PSRAM の反復リーク監視の穴**を明記すること)。
