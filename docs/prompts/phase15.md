# Phase 15: PSRAM 本番反映(WASM linear memory の PSRAM 移行)

## 目的

Phase 12 で PSRAM は「ハード・ドライバの水準では go」と判定したが、
本番反映は未実施のまま延期した。本フェーズで **WASM アプリが使えるメモリの
上限を internal ヒープの最大連続ブロックから切り離す**。

Phase 12 の結論は「PSRAM を有効化しても largest free block は 31,744 のままで
効果がない」だったが、**この判定は指標の取り方に問題がある可能性が高い**。
後述の事実 F1〜F3 のとおり、WASM の linear memory は WAMR のプールではなく
プラットフォーム層の `os_mmap` 経由でシステムヒープから取られており、かつ
WAMR の esp-idf 層には PSRAM 有効時に確保先を PSRAM へ切り替える経路が
存在する。largest free block は internal ヒープの指標なので、linear memory が
PSRAM から取れていても値は変わらない。**本フェーズはこの再判定から始める。**

副次の目的として、Phase 12 で未解明のまま残った「PSRAM 有効時に SDMMC
プローブが返らない」の真因切り分けを行う(ステップ 4)。

## 位置づけと前提

- セッション開始時に以下を通読すること。
  - `CLAUDE.md`、`docs/workflow.md`、`docs/lessons.md`
  - `docs/architecture.md` §9(メモリ配置方針)・§11-2(PSRAM)
  - `docs/results/phase10.md` の P10-4 節、`docs/results/phase12.md` 全体
  - `docs/results/phase13.md`(metronome の絶対値目標と実測値)
- **シェル操作はすべて `scripts/hpane.sh` による herdr ペイン経由**
  (`docs/workflow.md` §3)。直接実行しない。
- 回帰は `scripts/device-regress.sh`(Phase 12 で整備)を既定とする。
  マウント経路が SDSPI にフォールバックしていたら、回帰結果を出す前に停止して
  電源の入れ直しを要求すること(Phase 12 の教訓)。

## 既知の事実(調査済み。Claude Code は着手前にソースで再確認すること)

以下は WAMR 2.4.0 のソースと ESP-IDF 公式ドキュメントで確認済みの事実。
**推測ではなく前提として使ってよいが、実際に使っている
`managed_components/` 配下のバージョンで同じ記述になっているかは必ず確認すること。**

- **F1: linear memory はプールから取られない。**
  `core/iwasm/common/wasm_memory.c` の `wasm_allocate_linear_memory()` は、
  `WASM_MEM_ALLOC_WITH_USAGE`(既定 0)が無効なら
  `wasm_mmap_linear_memory()` → `os_mmap()` を呼ぶ。`Alloc_With_Pool` で
  64KB プールを渡していても、プールに入るのは module / instance / exec_env /
  globals / tables などのランタイム構造体のみ。
- **F2: esp-idf の `os_mmap` はシステムヒープを直接叩く。**
  `core/shared/platform/esp-idf/espidf_memmap.c` の `os_mmap()` は、
  非実行領域に対し `heap_caps_malloc(size, MALLOC_CAP_8BIT)` を呼ぶ。
  → largest free block 31,744 に当たっているのは linear memory。
- **F3: PSRAM 有効時に確保先を PSRAM へ切り替える経路がある。**
  `core/shared/platform/esp-idf/shared_platform.cmake` に
  `if(CONFIG_ESP32S3_SPIRAM_SUPPORT) add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1) endif()`
  があり、これが立つと `os_mmap` の確保フラグは実行領域・非実行領域とも
  `MALLOC_CAP_SPIRAM` に固定される。
  **ただし `CONFIG_ESP32S3_SPIRAM_SUPPORT` は IDF 5.0 で `CONFIG_SPIRAM` に
  改名された旧名であり、IDF 5.5.1 のビルドで CMake 変数として立つかは未確認。**
  (Espressif 自身の wasmachine_shell コンポーネントは 0.1.1 でこの名前を
  `CONFIG_SPIRAM` へ変更している。)
- **F4: 確保サイズは初期ページ数で決まる。**
  `map_size = init_page_count × num_bytes_per_page`(最大ページ数ではない)。
  つまり **PSRAM を用意しただけではアプリの使えるメモリは増えない**。
  Rust 側で `--initial-memory` / `--max-memory` を増やす必要がある。
- **F5: `memory.grow` は esp-idf では `os_mremap_slow`**
  (新規確保 → コピー → 旧解放)。成長の瞬間に新旧が同時に存在し、ピークは 2 倍。
- **F6: `.wasm` バッファは `wasm_runtime_unload` まで保持が必要で、かつ
  書き込み可能でなければならない**(ランタイムが内容を書き換える)。
  これはランチャー側の確保なので、WAMR とは独立に PSRAM へ移せる。
- **F7: `WAMR_BUILD_ALLOC_WITH_USAGE=1` + `Alloc_With_Allocator` で
  `Alloc_For_Runtime` と `Alloc_For_LinearMemory` を用途別に振り分けられる**
  (`core/iwasm/include/wasm_export.h` の `mem_alloc_usage_t`)。
  ただし **ESP-IDF コンポーネントの `build-scripts/esp-idf/wamr/CMakeLists.txt`
  はこのオプションを Kconfig に露出していない**。有効化には
  `managed_components/` の書き換えが必要で、これは `idf.py reconfigure` で
  消える(IDF 6 移行時の教訓)。**最後の手段とする。**
- **F8: ESP-IDF 側の制約**(公式ドキュメント「Support for External RAM」)。
  - `CONFIG_SPIRAM_USE` は 3 択。`CAPS_ALLOC` は PSRAM を heap_caps に登録し
    `MALLOC_CAP_DEFAULT` タグも付くが標準 `malloc()` は internal のまま。
    `USE_MALLOC` は `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` の閾値で
    internal/external の優先順を切り替える。
  - flash キャッシュ無効中(flash 書き込み中など)は PSRAM もアクセス不能。
  - DMA ディスクリプタは PSRAM に置けない。DMA の PSRAM 帯域は狭い。
  - 32KB を超えるデータを触るとキャッシュが足りず PSRAM 生速度に落ち、
    キャッシュ済み flash を追い出して以後のコード実行も遅くなる。
  - タスクスタックは既定で PSRAM に置かれない。

## ゲート(必須)

1. **ステップ 0(事実確認)の報告 → 承認**を経てからステップ 1 に進む。
2. **ステップ 1(設計メモ)の承認**を経てから実装に入る。
3. **Host API / ABI の変更は一切行わない。** 必要だと判断した場合は
   実装せずに提案として報告し、承認を待つ。
4. `docs/workflow.md` の手順・`scripts/` の変更は提案 → 承認。
5. **`managed_components/` の書き換えは、ステップ 0〜2 の手段がすべて
   不成立と確認されるまで行わない。** 行う場合も事前に承認を取り、
   `idf.py reconfigure` で消える前提の永続化方法(ベンダリング等)を
   セットで提案すること。

---

## ステップ 0: 事実確認(実装なし・承認ゲート)

**目的**: Phase 12 の「効果なし」判定をやり直し、以後の設計分岐を確定する。
コード変更はログ出力の追加のみに限る。

### O-1. 旧 config 名が立つかの机上確認

- `build/config/sdkconfig.cmake` および `build/config/sdkconfig.h` に
  `CONFIG_ESP32S3_SPIRAM_SUPPORT` が定義されているか(PSRAM 有効ビルドで)。
- `managed_components/` 配下の `shared_platform.cmake` に F3 の記述があるか。
  無ければ WAMR のバージョンが違うので、代わりに何があるかを報告する。
- ビルドログに `WASM_MEM_DUAL_BUS_MIRROR` が定義されているかを確認する
  (`-DWASM_MEM_DUAL_BUS_MIRROR=1` がコンパイル行に出るか)。

### O-2. linear memory の実アドレスを見る(決定的な実験)

`memory_instantiate` 直後、または `wasm_runtime_instantiate` 呼び出し側で、
**`memory->memory_data` に相当するポインタのアドレスと確保サイズをログ出力**する。
ランタイム内部に手を入れずに済ませたい場合は、WASM 側で先頭アドレスを
取得する方法でもよいが、native 側で直接取れるならそちらを優先する。

判定: ESP32-S3 では内部 DRAM が `0x3FC8xxxx` 台、PSRAM のデータ仮想アドレスが
`0x3C`〜`0x3D` 台。**先頭バイトを見れば PSRAM から取れているかが即断できる。**

以下 3 条件で取得し表にする。

| 条件 | PSRAM | SD 経路 | memory_data アドレス | サイズ |
|---|---|---|---|---|
| C0 | 無効(現行 main) | SDMMC | | |
| C1 | 有効 + `CONFIG_SPIRAM_USE=CAPS_ALLOC` | E5 迂回(SDSPI) | | |
| C2 | 有効 + `CONFIG_SPIRAM_USE=USE_MALLOC` | E5 迂回(SDSPI) | | |

### O-3. internal ヒープ領域の内訳を確定

各条件で `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)` を実行し、
**領域ごとの largest free block** を出す。
Phase 12 の「31,744 は独立した 32KB DRAM 領域の構造的上限」という説明が
領域境界によるものか、静的確保の並びによる断片化かを確定する。

### O-4. `.wasm` バッファと LVGL 描画バッファの確保箇所の棚卸し

- ランチャーが `.wasm` を読み込むバッファの確保関数と、そのサイズ(最大の app で何 B か)。
- LVGL の描画バッファの確保箇所とサイズ。
- **L0 キュー・テンポマップ・L1 の構造体が internal に固定されていることの確認**
  (静的 BSS か `MALLOC_CAP_INTERNAL` 明示確保か)。
  `CONFIG_SPIRAM_USE_MALLOC` を選んだ場合にこれらが PSRAM へ流出しないかを
  ここで判定する。§9 の方針が暗黙に崩れていないこと。

### O-5. アプリ側のメモリ宣言の確認

現行の `.wasm` 各本について、宣言されている初期ページ数・最大ページ数を
`wasm-objdump` 等で一覧化する。F4 のとおり、これを増やさない限り
PSRAM を用意しても使えるメモリは増えない。Rust 側でこれを制御する
リンカ引数の指定箇所(`.cargo/config.toml` か `build.rs` か)を特定する。

### ステップ 0 の報告物

`docs/results/phase15.md` に O-1〜O-5 の結果を表で記録し、
**「A 案(PSRAM 有効化のみで linear memory が PSRAM に行く)が成立するか」**
に yes/no で答える。ここで承認を取る。

---

## ステップ 1: 設計メモ(承認ゲート)

ステップ 0 の結果に応じて、以下から**採る手段を 1 つ選び**、根拠と
副作用を `docs/design/phase15-psram.md` に書く。複数を併用する場合は
その順序と理由も書く。

- **A 案**: PSRAM 有効化のみ(F3 の経路が成立する場合)。変更量は
  `sdkconfig.defaults` のみ。
- **B 案**: `CONFIG_SPIRAM_USE=CAPS_ALLOC` + **ランチャーの `.wasm` バッファを
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` に変更**。A と独立に効き、
  リスクが最も低い。F6 のとおり書き込み可能である必要があるので、
  読み取り専用マップにはしないこと。
- **C 案**: `CONFIG_SPIRAM_USE=USE_MALLOC` + `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`
  の閾値調整。**採る場合は O-4 の結果を根拠に、L0/L1 が internal に
  留まることを明示すること。**
- **D 案**: `WAMR_BUILD_ALLOC_WITH_USAGE=1` + `Alloc_With_Allocator`。
  **ゲート 5 に従い、A〜C がすべて不成立と確認された場合のみ。**

あわせて設計メモに書くこと:

- **PSRAM に置くもの / 置かないもの の表**。L0 キュー・テンポマップ・
  L1 の状態・DMA バッファ・タスクスタックは internal 固定(§9 を変えない)。
- **`CONFIG_SPIRAM_XIP_FROM_PSRAM` を採るか採らないか**の判断と根拠。
  採れば flash 操作中もキャッシュが無効化されないため、L0 まわりで
  `IRAM_ATTR` を使っている箇所の内部 RAM を節約できる可能性がある。
  ただしコード実行速度に影響するので、**採るなら別ステップに分離**し、
  本フェーズでは判断だけ書いて実施しない選択でもよい。
- **アプリの初期ページ数をいくつに引き上げるか**。1 本(検証用)だけを
  引き上げ、他は現状維持とする。

---

## ステップ 2: 実装

ステップ 1 で承認された手段のみを実装する。

- `sdkconfig.defaults`(または `sdkconfig.defaults.esp32s3`)に PSRAM 設定を入れ、
  **設定値ごとにコメントで根拠を書く**(80MHz octal は Phase 12 の 20/20 実績、等)。
- 検証用アプリを 1 本、初期ページ数を引き上げてビルドする。
  **新規アプリを追加せず、既存の 1 本を対象にする**(パーティション逼迫を避ける)。
- ログに、アプリ起動時の `memory_data` アドレス・サイズ・internal free・
  largest free block を出す。これは**常設**とし、以後の回帰で毎回見えるようにする。

## ステップ 3: 計測(絶対値で判定)

PSRAM 有効構成で、以下を測る。判定は **Phase 13 と同じ絶対値目標**で行う。
before/after 比較はしない。

- **T-1: metronome の MIDI クロック**。Phase 13 と同一条件(実機 MIDI OUT →
  UM-ONE → Linux PC / ALSA)で、
  **クロック欠落ゼロ / clocks・expected が 100% / BPM 分布が単峰 /
  平均間隔 20833µs ±10µs**。TX 側 σ も記録する。
  `seq_smoke` による計測系の妥当性確認を先に通すこと(Phase 13 の手順)。
- **T-2: `app_tick` の実行時間とジッタ**。数千サンプルの
  min / avg / max / 分布。Phase 4 の計測コード(`bench`)が使えるなら流用する。
  **PSRAM 化でここは必ず悪化する。悪化量を数値で記録することが目的**であり、
  T-1 が通っていれば T-2 の悪化そのものは不合格条件にしない。
  ただし app_tick の最大実行時間が 100ms tick 周期を脅かす水準なら報告して止める。
- **T-3: 引き上げた初期ページ数の .wasm が実際に起動すること**。
  **これが本フェーズの本当の合否**であり、largest free block の値ではない。
  どこまで引き上げると失敗するかの上限も 1 回探ること。
- **T-4: 20 回連続再起動で 20 回成功**(`device-regress.sh` で回す)。

## ステップ 4: SDMMC と PSRAM の共存(切り分け)

ステップ 3 まで完了してから着手する。Phase 12 の E5 は迂回であって
解明ではなく、迂回すると常に SDSPI(11.43MHz)固定になる。

- `docs/results/phase12.md` の H1〜H4 のうち、どこまで潰れているかを整理する。
  E5 が効いたことから H4(WDT の発火元が SD ではない)は否定されているはずだが、
  記録で確認すること。
- **H1(ピン競合)を机上で確定させる**。Waveshare ESP32-S3-Touch-LCD-2.8 の
  回路図の SD 配線(CLK / CMD / D0〜D3)と、octal PSRAM が占有する
  GPIO(33〜37 系)を突き合わせる。競合していれば、それが答えであり
  「この基板では SDMMC と octal PSRAM は両立しない」という結論を記録して終える。
- 競合していない場合のみ H2(初期化順序)・H3(速度組合せ)に実験を割り当てる。
  各仮説 1 実験、結果を表にする。
- **結論がどちらでも本フェーズは完了とする。** 両立不能なら
  「SDSPI 固定を受け入れる。将来サンプルプレーヤーで SD から波形を流す段で
  再検討する」と `docs/status.md` に明記する。
  SDSPI 固定時に internal の largest が 16,384B 減る件が、PSRAM 化後も
  WASM の起動を妨げないことは T-3 で確認済みであること。

## ステップ 5: 回帰と文書化

- 残っている全アプリで `device-regress.sh` を 1 回通し、
  WARN/ERROR なし・heap リークなしを確認(既知の許容パターンは設定ファイルどおり)。
- `docs/architecture.md` §11-2 を「延期」から現状に改訂する。
  §9 のメモリ配置方針に「PSRAM に置くもの / 置かないもの」の表を追記。
- `docs/results/phase15.md` に O-1〜O-5、T-1〜T-4、ステップ 4 の結果を集約。
- `docs/lessons.md` に最低限これを追加:
  - **largest free block は WASM の可否を表す指標ではない**。
    linear memory は `os_mmap` 経由で確保先が変わりうるので、
    判定は「実際に大きい .wasm が起動するか」と「memory_data のアドレス」で行う。
  - (ステップ 4 の結論に応じて)SDMMC と PSRAM の共存可否。
- **本フェーズは main を PSRAM 有効構成のまま残す**(Phase 12 と違い、本番反映である)。
  ただし T-1 が不合格なら PSRAM 無効に戻し、理由を記録して終える。

## スコープ外

- AOT 化(`WAMR_BUILD_AOT`、`wamrc`、XIP AOT)。
  F3 の経路により S3 + PSRAM では AOT コードも PSRAM に置かれ内部 RAM を
  食わない見込みだが、**本フェーズでは判断材料の記録に留め、実施しない**。
  インタプリタのまま T-1〜T-3 が通るなら AOT は不要である。
- Host API / ABI の変更、新規アプリの追加。
- 式ペダル(ADS1115)、ブラウザホスト、IDF 6 移行。
- LVGL 描画バッファの PSRAM 移動(O-4 で場所だけ特定し、実施は別フェーズ)。

## 完了条件

1. ステップ 0 の表が揃い、A〜D のどの手段を採るかが根拠付きで決まっている。
2. 初期ページ数を引き上げた `.wasm` が実機で起動する(T-3)。
3. metronome が Phase 13 と同じ絶対値目標を PSRAM 有効構成で満たす(T-1)。
4. 20 回連続再起動で 20 回成功(T-4)、全アプリ回帰が合格。
5. SDMMC と PSRAM の共存可否について、真因または「この基板では不能」の
   いずれかの結論が記録されている。
6. `docs/architecture.md` §9・§11-2 と `docs/lessons.md` が更新されている。
