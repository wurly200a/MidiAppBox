# Phase 12 実施記録 — 基盤整備(パーティション拡張・アプリ整理・実機テスト自動化・PSRAM 可否)

対応する指示書: `docs/prompts/phase12.md`
前提の実測値: `docs/results/phase11.md`(最終回帰)、`docs/results/phase10.md`(P10-4)

生データ: `captures/phase12/`(.gitignore 対象)

---

## 作業 1: パーティション拡張(2026-09-06)

### 変更内容

| ファイル | 変更 |
|---|---|
| `src/sdkconfig.defaults` | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` / `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"` / `CONFIG_PARTITION_TABLE_CUSTOM=y` / `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` を追記 |
| `src/partitions.csv` | **新規**(下表) |

`src/sdkconfig` は .gitignore 対象の生成物なので、削除して
`sdkconfig.defaults` から再生成した。再生成前後の差分は上記 4 項目
(`FLASHSIZE_2MB`→`FLASHSIZE_16MB`、`PARTITION_TABLE_SINGLE_APP`→`CUSTOM`、
`PARTITION_TABLE_FILENAME`)**のみ**で、他の設定はドリフトしていないことを
`diff` で確認済み。

### パーティション表(前後)

| | 変更前(`partitions_singleapp.csv` / 2MB) | 変更後(`partitions.csv` / 16MB) |
|---|---|---|
| nvs | data/nvs `0x9000` `0x6000`(24K) | **同一** |
| phy_init | data/phy `0xf000` `0x1000`(4K) | **同一** |
| factory | app/factory `0x10000` **`0x100000`(1M)** | app/factory `0x10000` **`0x400000`(4M)** |

**オフセットは 3 パーティションとも変更前と同一**で、`factory` のサイズだけが
1MB → 4MB に増えている。したがって **NVS の内容は失われない**(そもそも
NVS を使っているのは `app_main.cpp` の `nvs_flash_init()` だけで、アプリ設定等の
永続化データは現状ゼロ。`grep -rn "nvs_"` で確認済み)。

OTA は不要(無線スタックなし)なので factory 1 本のみ。

### 残容量(`idf.py size` / `check_sizes.py`)

| 項目 | 変更前 | 変更後 |
|---|---|---|
| バイナリサイズ | 0xfeff0 = 1,044,464 B | 0xfeff0 = 1,044,464 B(コード変更なし) |
| アプリパーティション | 0x100000 = 1,048,576 B | **0x400000 = 4,194,304 B** |
| 残り | **0x1010 = 4,112 B(0%)** | **0x301010 = 3,149,840 B(75%)** |

`idf.py size` の内訳(変更後):

| 区分 | サイズ |
|---|---|
| Flash Code `.text` | 634,818 |
| Flash Data `.rodata` | 306,676 |
| DIRAM(.bss/.text/.data) | 231,287(67.68%、残 110,473) |
| IRAM | 16,384(100%) |
| Total image | 1,044,345 |

**指示書の前提の訂正**: 「ファームウェア領域の逼迫は `.wasm` 埋め込みが主因」は
実測と合わない。埋め込み総量は `.wasm` 10 本 = **21,201 B**、MP3 アセット 3 本 =
**145,739 B**、合計 166,940 B で、これは `.rodata` の 54% ではあるが**イメージ全体の
16%** にすぎない。主因は Flash Code `.text` 634,818 B(ESP-IDF + LVGL + WAMR)である。
つまり**アプリ整理(作業 2)はフラッシュ容量対策としてはほぼ効かない**
(削除候補 4 本で 1,724 B)。作業 2 の価値は容量ではなく「回帰対象の削減」にある。

### 予約データ領域を切るかどうか — **切らない(実測に基づく判断)**

当初 `appdata, data, fat, 0x410000, 0x400000` を将来用に予約した表で 1 度
ビルド・フラッシュし、起動時の free heap を比較した:

| 構成 | `Audio_Init` 前 | `Audio_Init` 後 | `runtime ready` |
|---|---|---|---|
| Phase 11 最終(2MB / single app) | 151,428 | 104,224 | 97,800 |
| 16MB / factory 4M + **appdata 予約あり** | 151,372 | 104,168 | 97,744 |
| 16MB / factory 4M(**予約なし・採用**) | **151,428** | **104,224** | **97,800** |

**パーティションエントリ 1 個につき internal heap が 56 B 恒久的に減る**
(`esp_partition` のランタイムリストがヒープ上に作られるため)。予約は現時点で
消費者がおらず(アプリは SD 上に置く方針)、`factory` の後ろは空き番地なので
**後から追加してもどのオフセットも動かない**。よって予約せず、
**free heap を Phase 11 基準とビット一致させる**方を採った。

### 起動確認(実機)

生データ: `captures/phase12/monitor-work1.log`

- ブートローダの Partition Table ログが nvs / phy_init / factory の 3 本を表示。
  `esptool.py read_flash 0x8000` で実フラッシュからも同じ表を読み出して確認
  (`captures/phase12/pt_readback.bin`)。
- **`idf.py erase-flash` は不要だった。** 既存 3 パーティションのオフセットが
  同一なので、通常の `idf.py flash`(bootloader + partition-table + app)だけで
  移行できる。SD カードにも一切影響しない。
- 起動時 free heap は **Phase 11 最終回帰と完全一致**(上表)。
- **既知の起動警告が 1 件消えた**:
  `spi_flash: Detected size(16384k) larger than the size in the binary image
  header(2048k)` はイメージヘッダのフラッシュサイズが 16MB になったため出なくなった。
  以後の回帰ではこの行を「既知の許容パターン」から外してよい。
- WARN/ERROR 0 件、SD マウント正常、`menu: 8 app(s) listed`。

---

## 作業 2: アプリ整理(2026-09-06)

削除前のタグ: **`pre-app-prune`**(コミット `51b6c3c`)

### 調査: アプリ × Host API カバレッジ表

`wasm-apps/*/src/*.rs` の `extern "C"` 宣言と本文の呼び出しから機械抽出(Host API 全 28 関数)。

| API | hello | demo | bars | bench | touch_demo | mp3player | clicktest | metronome | midi_loopback | seq_smoke |
|---|---|---|---|---|---|---|---|---|---|---|
| draw_text | . | X | X | . | X | X | X | X | X | X |
| fill_rect | . | X | X | . | X | X | X | X | X | X |
| poll_event | . | . | . | . | X | X | X | X | X | X |
| audio_play | . | . | . | . | . | **X** | . | . | . | . |
| audio_ctrl | . | . | . | . | . | **X** | . | . | . | . |
| audio_get_state | . | . | . | . | . | **X** | . | . | . | . |
| fs_list | . | . | . | . | . | **X** | . | . | . | . |
| audio_set_volume | . | . | . | . | . | X | . | X | . | . |
| play_click | . | X | X | . | X | . | X | . | . | . |
| now_ms | . | X | X | X | . | . | X | X | X | . |
| click_schedule | . | . | . | . | . | . | X | X | X | . |
| tone_define | . | . | . | . | . | . | . | X | . | X |
| tone_play | . | . | . | . | . | . | . | . | . | . |
| tone_schedule | . | . | . | . | . | . | . | **X** | . | . |
| midi_send | . | . | . | . | . | . | . | X | X | X |
| midi_recv | . | . | . | . | . | . | . | . | X | X |
| transport_start / stop / continue / locate / get_position | . | . | . | . | . | . | . | . | . | **X** |
| tempomap_set_tempo / set_meter / set_loop | . | . | . | . | . | . | . | . | . | **X** |
| seq_write / seq_flush_after / seq_filled_until | . | . | . | . | . | . | . | . | . | **X** |
| time_us_to_tick | . | . | . | . | . | . | . | . | . | **X** |

`tone_play` は**どのアプリも呼んでいない**(`play_click` ≡ `tone_play(0)` は別シンボル)。
移行ステップ 4b で旧発音 API の整理をするときの入力になる。

### 判断(残す基準 = 固有カバレッジ、または製品・計測器としての固有役割)

| アプリ | 固有 API | 判定 |
|---|---|---|
| mp3player | audio_play / audio_ctrl / audio_get_state / fs_list | 残す(SD + オーディオ経路) |
| metronome | tone_schedule | 残す(製品) |
| seq_smoke | 新 API 12 関数すべて | 残す(新 API の恒久スモーク) |
| midi_loopback | なし(midi_recv は seq_smoke と重複) | 残す(E1 統計 = 計測器としての固有役割) |
| clicktest | なし | 残す(旧 `click_schedule` 経路の回帰対象。移行ステップ 4b まで) |
| touch_demo | なし(現時点では clicktest が上位集合) | 残す(下記) |
| demo | なし。API 集合が bars と**完全一致**、clicktest の真部分集合 | **削除** |
| bars | なし。API 集合が demo と完全一致 | **削除** |
| bench | now_ms のみ。ランチャーから起動不可(native `run_bench()` 専用) | **削除** |
| hello | **ゼロ**(`app_init()` が 42 を返すだけ。native `run_selftest()` 専用) | **削除** |

**demo と touch_demo のどちらを残すか**: 現時点ではどちらも clicktest の真部分集合で、
基準だけでは決まらない。決め手は clicktest が消えた後(移行ステップ 4b)の残余カバレッジ:

- touch_demo を残す → `poll_event` + `play_click` が残る
- demo を残す → `now_ms` + `play_click` が残るが、`now_ms` は metronome / midi_loopback が既に覆う

**touch_demo が厳密に上位**なので touch_demo を残した。加えてタッチパネルの座標・
DOWN/UP を可視化する診断ツールという固有の役割がある。

### 実施内容

削除 4 本(hello / demo / bars / bench)、残り **6 本**
(touch_demo / mp3player / clicktest / metronome / midi_loopback / seq_smoke)。

| 対象 | 変更 |
|---|---|
| `wasm-apps/{hello,demo,bars,bench}/` | ディレクトリごと削除 |
| `src/components/wasm_runtime/CMakeLists.txt` | `EMBED_FILES` から 4 本を削除 |
| `src/components/wasm_runtime/launcher.cpp` | demo / bars の extern 宣言と seed を削除。seq_smoke のコメントを「恒久スモーク」に更新 |
| `src/components/wasm_runtime/launcher.hpp` | `launcher_run_cycle_test()` の説明を実装どおり mp3player.wasm に訂正(demo.wasm と書かれていた) |
| `src/components/wasm_runtime/wasm_runtime.{cpp,hpp}` | hello / bench 専用の native ハーネス `run_selftest()` / `run_selftest_task()` / `run_bench()` / `run_bench_module()` を削除(**いずれも呼び出し元ゼロ**)。`esp_cpu.h` も不要になり削除 |
| `CLAUDE.md` / `docs/architecture.md` §10 / `wasm-apps/README.md` / `hosts/linux/README.md` | 回帰対象リストとアプリ一覧を 6 本に更新。README に seq_smoke の行を追加 |

**削除時に踏んだ落とし穴**: `wasm_runtime.cpp` の「Phase 4: 計測」節には bench 専用の
`run_bench_module()` と、**現在も常設で使われている** tick ジッタ統計
(`kJitterSamples` / `s_intervals_us` / `s_durations_us` / `log_stats`)が同じ無名名前空間に
同居していた。節ごと消してビルドエラーになり、ジッタ統計だけを
「`// ---- tick ジッタ計測(常設。Phase 4 §2 由来)----`」として復元した。

### SD 上の seed 残骸 — **掃除しない**(指示書の既定を採用)

`/sdcard/apps/demo.wasm` と `bars.wasm` は seed をやめても消えないため、ランチャーには
引き続き 8 本表示される(`menu: 8 app(s) listed`)。ユーザーが SD に置いたアプリを
ホストが勝手に消さない方針を優先した。作業 3 の自動回帰はアプリ名を明示指定するので
残骸の影響を受けない。

### バイナリサイズ

| | サイズ | 残り(4MB partition) |
|---|---|---|
| 削除前 | 0xfeff0 = 1,044,464 B | 3,149,840 B |
| 削除後 | **0xfe620 = 1,041,952 B** | **3,152,352 B** |

**−2,512 B**(`.wasm` 4 本 1,724 B + 死にコードのネイティブ分)。作業 1 で述べたとおり
容量への寄与はごく小さく、この作業の価値は回帰対象の削減にある。

### 回帰(作業 1 + 作業 2 をまとめて 1 回)

ユーザー合意により、作業 1 は設定のみの変更で起動時 free heap が Phase 11 基準と
ビット一致していたため、手動回帰は作業 2 の削除後に 1 回だけ実施した
(作業 3 の「手動回帰と同じ値が出ること」の比較対象にもそのまま使う)。

**実機 — 合格**(生データ: `captures/phase12/monitor-regress.log`。ループバック配線なし)

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block |
|---|---|---|---|---|
| touch_demo | 54052 | 54052 | +0 | 31744 |
| mp3player | 54052 | 54008 | −44 | 31744 |
| clicktest | 54008 | 54008 | +0 | 31744 |
| metronome | 54008 | 54008 | +0 | 31744 |
| midi_loopback | 54008 | 54008 | +0 | 31744 |
| seq_smoke | 54008 | 54008 | +0 | 31744 |

- **Phase 11 最終回帰の基準値(54052 / 54008、largest block 31744、mp3player の −44B)と
  完全一致。** リークは 0。
- **WARN/ERROR 0 件**(`^[WE] (` 行がゼロ)。`no free slot` / `MIDI RX: ring buffer full` も 0。
- 起動時基準値も一致: `Audio_Init: 151428 -> 104224`、`runtime ready ... free heap 97800`。

**Linux ホスト — 合格**(生データ: `captures/phase12/linuxreg/*.log`。単発実行モード + ESC 終了を自動化)

| アプリ | app started | app stopped | 警告/エラー行 |
|---|---|---|---|
| touch_demo / mp3player / clicktest / metronome / midi_loopback / seq_smoke | 各 1 | 各 1 | 各 **0** |

残留プロセスなし。今回は ALSA も使えており、Phase 11 で出ていた
`/dev/snd/seq Permission denied` の 2 行も出ていない。
