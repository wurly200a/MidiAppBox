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

**追記(2026-09-06、Phase 14)**: 上表は Phase 12 時点(2026-09-06 朝)のスナップショット。
その後 metronome(Phase 13)・midi_loopback(Phase 14 ステップ1)が `click_schedule` /
`tone_schedule` / `midi_send` の Start/Stop 副作用から音楽時間軸 API へ移行し、
clicktest は Phase 14 ステップ2で削除された(利用者ゼロ確認後)。
`click_schedule` 行・`tone_schedule` 行・`clicktest` 列は削除後は該当なしになる
(API 自体もステップ3で削除。詳細・削除後のカバレッジ確認は `docs/results/phase14.md`)。
歴史的な決定根拠として本表はそのまま残す。

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

---

## 作業 3: 実機テストの自動化(2026-09-06)

### 事前調査 1: シリアル入力の経路 — **`idf.py monitor` 経由でそのまま届く**

現行のコンソール構成を先に確認した:

| 項目 | 値 |
|---|---|
| primary console | **UART0**(`CONFIG_ESP_CONSOLE_UART_DEFAULT`、UART_NUM 0、115200) |
| secondary console | **USB Serial/JTAG**(`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`) |
| `/dev/ttyACM0` の実体 | ESP32-S3 内蔵 USB Serial/JTAG(起動ログ `rst:0x15 (USB_UART_CHIP_RESET)`) |

ESP-IDF の secondary console は**出力専用**なので、`/dev/ttyACM0` へ送った文字は
stdin(= UART0)には届かない。そこで **コンソール設定は変更せず、USJ ドライバを
直接入れて読む**方式を `#ifdef PHASE12_SERIAL_PROBE` で実測した(検証後に削除済み)。

| 確認項目 | 実測結果 |
|---|---|
| `usb_serial_jtag_driver_install()` | `ESP_OK`。**ログ出力に影響なし**(secondary console の出力はそのまま出る) |
| `hpane.sh send esp32-monitor "<text>"` の到達 | **届く**(pane の PTY → `docker run -it` stdin → `idf.py monitor` → シリアル → 実機) |
| 行末 | **CR(0x0D)のみ**。LF は来ない |
| 38 文字 × 5 行を待ちなしで連続送信 | **バイト欠落 0** |

→ 代替案(`esp_console` REPL / pyserial 直叩き / コンソールを USJ へ切替)は**いずれも不要**。
コンソール設定を触らないので、UART0 のログ経路・フラッシュ・リセット挙動に影響しない。

### 事前調査 2〜4 と実装方針(ユーザー承認済み)

- コマンド集合: `ping` / `ls` / `run <app>` / `stop` / `heap`。
  **応答は `ESP_LOG`(タグ `MBCMD`)で出す。** `printf`(stdout)は primary console
  = UART0 に出てしまい USB 側に現れないため。
- アプリ内 UI 操作の自動化は v1 のスコープ外(設計案は下記)。
- ログの機械判定は `scripts/device-regress.conf` に外出し。

### 実装

| ファイル | 内容 |
|---|---|
| `src/main/serial_cmd.{hpp,cpp}` | **新規**。USJ ドライバ直読みのコマンドコンソール。`CONFIG_MIDIBOX_SERIAL_CMD`(既定 y)で有効化 |
| `src/main/Kconfig.projbuild` | `MIDIBOX_SERIAL_CMD` を追加 |
| `src/main/app_main.cpp` / `CMakeLists.txt` | `serialcmd::Init()` を SD 準備の後に呼ぶ。`esp_driver_usb_serial_jtag` を依存に追加 |
| `src/components/wasm_runtime/launcher.{hpp,cpp}` | `launcher_launch_by_name()` を追加(メニューのタップを経由しない起動) |
| `scripts/device-regress.sh` / `.conf` | **新規**。自動回帰スクリプトと設定 |
| `docs/workflow.md` | §2.2 と §3.4 に自動回帰を追記(以後の既定) |

設計上の制約の充足:

- コンソールタスクの優先度は **2**(audio_player の 3 より低い。教訓 P10-1)。
- スタックは **静的確保**(`xTaskCreateStatic` + 静的 3KB)。恒久物をヒープから取ると
  最大連続ブロックを分断する(教訓 6B / 7B-fix)。
- ロックは LVGL のもの(`launcher_show` と同じ流儀で任意タスクから `lvgl_port_lock(0)`)
  だけ。**L0 ディスパッチャの portMUX とは共有しない。**
- **タッチ・電源キーの既存操作系は一切変更していない。**

### 待ち方 — スクロールバック誤マッチの排除(§5 の手続きで承認済み)

`docs/workflow.md` §3.2 は常駐モニタの待ちに `waitfor`(= `herdr wait output`)を
使っているが、**本フェーズでこの失敗を実際に踏んだ**: モニタ再起動直後の
`waitfor "app(s) listed"` が**ペインのスクロールバックに残る前回の起動ログに誤マッチ**し、
まだ起動していないのに起動したと誤判定した(その結果、古いパーティション表のログを
新しい起動だと読み違えかけた)。§1-2 が禁じている失敗モードそのものである。

`scripts/device-regress.sh` の待ちは、ペイン出力ではなく **`tee` が書くログファイルの
「今回の待ちを始めた行より後ろ」**に対してのみ行う(`wait_line <pat> <from> <timeout>`)。
これで古い行への誤マッチが原理的に起こらない。ペインは人間が見るライブ表示として残す。

### 重要な発見: mp3player の「既知の −44B」は**再生経路でのみ出る**

自動回帰の初回実行で mp3player だけが `+0` になり、手動回帰の `−44` と食い違った。
手動回帰のログを見直すと、**mp3player 実行中にユーザーのタップ(= 再生開始)が
入っていた**(`TOUCH_CST328` の行が起動 12.5 秒後に並ぶ)。

検証: **無操作のまま 35 秒保持**して起動→停止したところ、`+0`(生データ:
`captures/phase12-mp3check/`)。`mp3player` の `app_init()` は `hostapi_fs_list` と
描画しかせず、再生は `handle_tap()` 経由でしか始まらない。

**結論**: −44B は「アプリの起動/停止のリーク」ではなく **MP3 再生経路の挙動**である。
Phase 9c 以降ずっと「既知の −44B」として扱ってきたが、その発生条件が特定できたのは今回が初めて。
`scripts/device-regress.conf` の `EXPECT_DELTA` は空にし(全アプリ 0 を要求)、
**再生経路まで含めた回帰が要るときは `docs/workflow.md` §3.3 の人間操作+カメラで行う**
(そこでは −44B が出るのが正常)ことをコメントに明記した。

### メモリ基準値の更新(シリアルコマンドを既定 y にしたことによる一度きりの移動)

| 測定点 | 作業 2 まで | 作業 3 以降 | 差分 |
|---|---|---|---|
| `Audio_Init` 前 free heap | 151,428 | **147,596** | −3,832(静的 BSS: 3KB スタック + TCB + 行バッファ) |
| `runtime ready` 時 free heap | 97,800 | **93,968** | −3,832 |
| アプリ実行時の free heap | 54,008 | **49,160** | −4,848(上記 + USJ ドライバのリングバッファ約 1,016B) |
| **アプリ実行時の largest free block** | **31,744** | **31,744** | **±0** |

**最大連続ブロックは不変**(静的確保にした狙いどおり)。以後の回帰はこの新基準で行う。

### 自動回帰の結果 — **PASS**(生データ: `captures/phase12-auto/`)

`./scripts/device-regress.sh --task phase12-auto` を 1 回実行。**物理操作なしで完走**。

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block | 判定 |
|---|---|---|---|---|---|
| touch_demo | 49160 | 49160 | +0 | 31744 | PASS |
| mp3player | 49160 | 49160 | +0 | 31744 | PASS |
| clicktest | 49160 | 49160 | +0 | 31744 | PASS |
| metronome | 49160 | 49160 | +0 | 31744 | PASS |
| midi_loopback | 49160 | 49160 | +0 | 31744 | PASS |
| seq_smoke | 49160 | 49160 | +0 | 31744 | PASS |

許容外の WARN/ERROR: **0 件**。所要時間は約 100 秒(保持 6 秒 × 5 + 20 秒 + 起動待ち)。

### 手動回帰との一致確認

| 項目 | 手動(作業 2) | 自動(作業 3) | 一致 |
|---|---|---|---|
| アプリごとの free heap 差分 | +0(mp3player のみ −44) | 全アプリ +0 | **条件差として説明済み**(上記のとおり −44 は再生操作でのみ発生。自動側は再生しない) |
| largest free block | 31744 | 31744 | ✔ |
| WARN/ERROR | 0 | 0 | ✔ |
| free heap の絶対値 | 54052 / 54008 | 49160 | シリアルコマンド分の一度きりの移動(上表) |

### アプリ内 UI 操作の自動化 — 設計案(別課題)

- 案: シリアルコマンド `ev <type> <param> <x> <y>` で `hostapi_poll_event` が読む
  イベントキューへ `hostapi_event_t` を直接積む。**Host API にも L0/L1 にも触れず、
  アプリ側も無改造**で済む。
- 問題: これは「テストがアプリの画面レイアウト(ボタン座標)に結合する」ことを意味する。
  実用化にはアプリがボタンの論理名と矩形を公開する仕組み(例: `app_ui_map()` エクスポート)が
  要り、それ自体が Host API の設計課題になる。**別課題として切り出す。**

---

## 作業 4: PSRAM の使用可否(2026-09-06)— 判定 **条件付き go**

生データ: `captures/phase12/psram-e2.log` / `psram-e3.log` / `psram-e5.log` /
`psram-bench.log` / `reboot-loop/boot1..20.log`、`captures/phase12-psram-internal/`。

### 仮説と実験の対応表

| 実験 | 仮説 | 条件 | 結果 |
|---|---|---|---|
| E1 | **H1 ピン競合** | 静的解析(`board_pins.hpp` + `audio.cpp`) | **否定** |
| E2 | **H4 WDT は SD 以外で発火?** | PSRAM(OCT / 80MHz / `SPIRAM_USE_MALLOC`)= P10-4 と同一 | **否定**(P10-4 を完全再現、ハングは SD で確定) |
| E3 | **H5 PSRAM 由来バッファが DMA 経路へ**(H2 の一種) | 上記 + `CONFIG_SPIRAM_USE_CAPS_ALLOC` | **否定**(同一の失敗) |
| E5 | **SDMMC プローブ自体が原因** | PSRAM(同上)+ **SDMMC プローブをスキップ** | **成立**(正常起動) |
| E6 | go 基準 1: 連続再起動 | E5 構成で 20 回 | **20/20 成功** |
| E7 | go 基準 2: 既存アプリの回帰 | E5 構成で `device-regress.sh` + INTERNAL heap 実測 | **劣化なし**(下記) |
| E8 | PSRAM レイテンシ(P10-4 で未取得) | E5 構成 + 16B ランダム読み書きベンチ | **取得**(下記) |
| — | go 基準 3: `midi_loopback` E1 のタイミング影響 | ループバック配線 + アプリ内タップが必要 | **別フェーズへ繰り越し**(ユーザー判断) |

H3(`SPIRAM_MODE` / `SPIRAM_SPEED` の組み合わせ)は、E5 が **80MHz / OCT のまま成功した**
ことで検証不要になった(速度・モードの問題ではない)。

### E1: ピン競合 — 否定

本ボードが使う GPIO(`board_pins.hpp` + `audio.cpp`):

`0, 1, 2, 3, 4, 5, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 47, 48`

ESP32-S3 の octal PSRAM が占有するのは **GPIO 33〜37**(SPIIO4〜7 + SPIDQS)で、
**1 本も重ならない**。SD は SDMMC/SPI とも 14/16/17/21。
(指示書は「GPIO35〜37」としていたが正しくは 33〜37。いずれにせよ競合なし。)

### E2: P10-4 の完全再現とハング箇所の特定

PSRAM 側は**完全に正常**:

```
octal_psram: vendor id 0x0d (AP), density 64 Mbit, 3V, Readlatency 10 cycles
esp_psram: Found 8MB PSRAM device / Speed: 80MHz
esp_psram: SPI SRAM memory test OK
esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
APP: Audio_Init: free heap 8506159 -> 8458955
```

そのうえで **毎回** `SDCARD: Trying SDMMC host: CLK=14 CMD=17 D0=16` の直後で停止し、
**パニックのバックトレースを一切出さずに** `rst:0x8 (TG1WDT_SYS_RST)` でリブートする
(4 回のブートすべて同一)。バックトレースが出ないことから、割り込み禁止区間または
クリティカルセクション内で回り続けていると読める。**H4 は否定**(SD 以外の初期化ではない)。

### E3: メモリ配置は原因ではない

`CONFIG_SPIRAM_USE_CAPS_ALLOC`(`malloc()` が PSRAM を返さず、PSRAM は
`heap_caps_malloc(MALLOC_CAP_SPIRAM)` でしか取れない)にしても、**9 回のブートすべてで
同一の失敗**。「DMA 不可の PSRAM バッファが SD ドライバに渡って固まる」という筋は消えた。

### E5: 原因の特定 — **SDMMC プローブ**

`sdcard.cpp` の SDMMC プローブ(`#ifdef PIN_SDMMC_CLK` のブロック)を飛ばして
SDSPI に直行させたところ、**PSRAM を有効にしたまま正常起動した**:

```
SDCARD: Using SPI host=2 MOSI=17 MISO=16 SCLK=14 CS=21
Name: USD00 / Size: 30250MB
MBCMD: ready
WASM/LAUNCH: menu: 8 app(s) listed
```

このプローブは **本ボードでは一度も成功したことがなく、失敗して SDSPI へ
フォールバックするためだけに存在する**(P10-4 の記述どおり)。PSRAM を有効にすると、
このプローブが「失敗して戻る」代わりに**戻ってこなくなる**のが真因である。

> **P10-4 はこの一歩手前まで来ていた。** 当時のパッチ
> (`captures/phase10/p10_4_test_code.patch`)には `PHASE10_MEMAUDIT_SKIP_SDMMC` という
> スキップ用マクロが**コメントアウトのまま**入っており、有効化して試す前に撤退していた。

**解消に必要な変更の規模**: プローブを飛ばすこと自体は 1 行だが、**それだけでは足りない**。
最終回帰で判明したとおり(下記「SD の初期化経路が largest free block を決めている」)、
SDMMC プローブを飛ばすと常に SDSPI 経路になり、no-PSRAM ではその構成で
最大連続ブロックが 15,360 まで落ちて WASM が起動できなくなる。**PSRAM の本番反映は
「SDMMC プローブの扱い」と「SDSPI 経路のメモリ消費対策」をセットで扱う必要がある。**

### E6: 連続再起動 20 回 — **20/20 成功**

E5 構成で、モニタ起動によるリセットを 20 回繰り返し、毎回
「PSRAM 8MB 認識 → SD マウント(30250MB)→ ランチャー表示」に到達し、
**TG1WDT リセットは 0 件**。go 基準 1 を満たす。

### E7: 既存アプリの回帰 — 劣化なし。ただし **largest free block は増えない**

PSRAM を有効にすると `esp_get_free_heap_size()` と `MALLOC_CAP_DEFAULT` が PSRAM を
含んでしまい `app: stopped` の値が比較にならない(free 8.4MB / largest 8.2MB と出る)。
そこで**シリアルコマンド `heap`(`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`)で測り直した**。

| 構成 | ランチャー待機時の INTERNAL free | INTERNAL largest free block |
|---|---|---|
| PSRAM 無効(main) | **64,276** | **31,744** |
| PSRAM 有効(E5 構成) | **106,763**(アプリ実行後も 106,599 で安定) | **31,744** |

- 内部 RAM の空きは **+42,487 B** 増える(LVGL バッファ等が PSRAM へ移るため)。
- **しかし largest free block は 31,744 で完全に同一**。WARN/ERROR 0 件、
  アプリごとの free heap 差分も 0。
- 全アプリの `device-regress.sh` 実行でも WARN/ERROR 0 件。

**これは本フェーズの動機に対する重要な否定的結果である。** 31,744 は独立した
32KB の DRAM 領域が与える構造的な上限で、内部 RAM がいくら空いても伸びない。
WASM の linear memory 確保が見ているのはまさにこの最大連続ブロックなので、
**PSRAM を有効にするだけでは linear memory の逼迫は 1 バイトも緩和されない。**
緩和するには **WAMR プール / linear memory を明示的に PSRAM へ置く**必要がある
(= 別フェーズの本番反映作業そのもの)。

### E8: PSRAM の 16B ランダムアクセス実測(P10-4 で未取得だった値)

L0 キュー操作を模した read 16B → write 16B を 8192 回。`-Og` ビルドなので
絶対値は悲観的、比率で読むこと(P10-4 と同じ条件)。

| 配置 | ns/op(4 回) | internal 比 |
|---|---|---|
| internal 2KB(BSS) | 362.4 / 364.4 / 413.6 / 362.7 | 1.00 |
| **PSRAM 4KB**(キャッシュ内) | 375.9 / 373.5 / 375.4 / 375.0 | **1.03** |
| **PSRAM 256KB**(キャッシュ超え) | 879.0 / 965.9 / 874.9 / 872.4 | **2.4〜2.7** |

- キャッシュに収まる範囲なら PSRAM は internal と実質同等(+3%)。
- キャッシュを超えると約 2.5 倍。ただしこれは **P10-4 が internal で観測した
  負荷時の最悪値 1385 ns/op より速い**。
- 設計根拠表(`docs/architecture.md` §12)への追記に使える。

### go / no-go 判定 — **条件付き go**

| go 基準 | 結果 |
|---|---|
| PSRAM + SD + アプリ起動が 20 回連続で成功 | **満たす**(E6: 20/20) |
| 既存アプリの回帰に劣化がない | **満たす**(E7: WARN/ERROR 0、差分 0、largest 不変) |
| largest free block が増えるはず | **増えなかった**(31,744 のまま。理由は上記) |
| `midi_loopback` E1 のタイミング影響 | **未実施**(別フェーズへ繰り越し。ユーザー判断) |
| PSRAM レイテンシの実測値 | **取得**(E8) |

**判定: 条件付き go。** PSRAM は本ボードで問題なく使える(真因は SDMMC プローブで、
修正は 1 行)。ただし **PSRAM を有効にするだけでは本フェーズの動機(WASM linear
memory の逼迫緩和)は達成されない**ので、本番反映は「WAMR プール / linear memory を
PSRAM へ移す」ところまでを一体で行う別フェーズとする。その別フェーズの完了条件に
**`midi_loopback` E1 によるタイミング検証**(本フェーズで繰り越した項目)を含めること。

### 後片付け

- 検証専用コード(`PHASE12_SERIAL_PROBE` / `PHASE12_NO_SDMMC_PROBE` / `PHASE12_PSRAM_TEST`)
  と `sdkconfig.defaults` の PSRAM 設定はすべて削除済み。`git grep PHASE12 -- src scripts` で残存なし。
- main は **no-PSRAM 構成**に戻し、リビルド・フラッシュ・自動回帰で確認済み。

---

## 最終回帰と、その過程で判明した重大な事実(2026-09-06)

### SD の初期化経路が largest free block を決めている

作業 4 の後始末(main を no-PSRAM に戻す)を終えて最終回帰を回したところ、**全アプリが
`instantiate: WASM module instantiate failed: allocate linear memory failed` で起動失敗**
した。ファームウェアは無関係で、起動直後のヒープは合格時と 1 バイト単位で同一だった
(`Audio_Init: 147596 -> 100392`、`heap after seq init: free 94212, largest block 53248`)。

分岐していたのは **SD がどの経路でマウントされたか**である:

| ログ | SD 経路 | ネゴ速度 | アプリ実行時 largest block | 結果 |
|---|---|---|---|---|
| `monitor-work1` / `monitor-regress` / `phase12-auto` | **SDMMC** | 20.00 MHz | **31,744** | 全アプリ OK |
| `phase12-nopsram-internal` / `phase12-final`(1 回目) | **SDSPI**(フォールバック) | 11.43 MHz | **15,360** | **全アプリ NG** |

SDSPI 経路に落ちると **最大連続ブロックがちょうど 16,384 B 減って 15,360** になり、
WASM の linear memory(約 20KB 連続)が確保できなくなる。教訓 9c の「largest free block
約 15KB で破綻」と同一水準である。

```
E (1361) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
E (1361) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
W (1361) SDCARD: SDMMC mount failed: 263, falling back to SDSPI
```

### P10-4 の記述を訂正する

`docs/results/phase10.md` P10-4 の

> PSRAM 無効時はこのプローブが失敗して SDSPI へフォールバックしており、
> 実際に使われるのは SPI 経路のみである

は**現状と合っていない**。本フェーズの合格ログはすべて **SDMMC 20MHz でマウントに成功**
している。つまりこの機体は普段 SDMMC で動いており、**SDSPI は「落ちると WASM アプリが
動かなくなる」経路**である。作業 4 の E5 で「1 行で直る」と書いた見立ては、この事実に
照らして**撤回**した(上記 E5 の節に反映済み)。

### 復旧方法: ボードの電源を入れ直す(ソフトリセットでは駄目)

SDMMC 初期化が失敗し始めたのは、作業 4 の PSRAM 実験で **SD プローブ中のリブートループを
何十回も起こした直後**からである。`idf.py monitor` の再起動は RTS/DTR によるソフトリセットで
**SD カードの電源は落ちない**ため、カードが応答しない状態から復帰しない。

**USB を抜き差しして電源を入れ直したところ、SDMMC 20MHz マウントに復帰し、回帰も一発で
合格した。** ソフトリセットを何度繰り返しても直らなかったので、この区別は重要である。

### 最終自動回帰 — **PASS**(生データ: `captures/phase12-final/`)

main(no-PSRAM、作業 1〜3 の成果物)に対し `./scripts/device-regress.sh --task phase12-final`
を 1 回実行。**物理操作なしで完走。**

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block | 判定 |
|---|---|---|---|---|---|
| touch_demo | 49160 | 49160 | +0 | 31744 | PASS |
| mp3player | 49160 | 49160 | +0 | 31744 | PASS |
| clicktest | 49160 | 49160 | +0 | 31744 | PASS |
| metronome | 49160 | 49160 | +0 | 31744 | PASS |
| midi_loopback | 49160 | 49160 | +0 | 31744 | PASS |
| seq_smoke | 49160 | 49160 | +0 | 31744 | PASS |

許容外の WARN/ERROR: **0 件**。

## Phase 13 への申し送り

1. **回帰は `./scripts/device-regress.sh --task <名前>` を既定にする**(`docs/workflow.md`
   §2.2 / §3.4)。物理操作なしで約 100 秒。対象アプリ・許容値は
   `scripts/device-regress.conf`。音・画面・アプリ内 UI 操作の確認が要るときだけ §3.3 の
   人間操作+カメラを使う(mp3player の再生経路 = 既知の −44B もそちら側)。
2. **メモリ基準値**(シリアルコンソール込み、no-PSRAM):
   `Audio_Init 147596 -> 100392` / `runtime ready 93968` / アプリ実行時
   **free heap 49160、largest free block 31744**。INTERNAL free はランチャー待機時 64,276。
3. **フラッシュ残容量は 3.15MB(75%)**。当面気にしなくてよい。
4. **`allocate linear memory failed` が出たらまず SD の経路を疑うこと。**
   `Speed: 20.00 MHz` = SDMMC(正常、largest 31744)、`11.43 MHz` + `falling back to
   SDSPI` = 異常(largest 15360、WASM 起動不能)。**復旧は USB の抜き差しによる電源断**で、
   ソフトリセットでは直らない。
5. **PSRAM は条件付き go**。真因は SDMMC プローブだが、プローブを外すと SDSPI 固定になり
   上記 4 の破綻を招くため、本番反映は「SDMMC プローブの扱い + SDSPI 経路のメモリ消費対策 +
   WAMR プール / linear memory の PSRAM 移動」を一体で行う別フェーズとする。
   その完了条件に **`midi_loopback` E1 によるタイミング検証**(本フェーズで繰り越し)を含める。
   **PSRAM を有効にするだけでは largest free block は 31,744 のまま増えない**ことに注意。
