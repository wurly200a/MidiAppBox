# Phase 11 実施記録 — 新アーキテクチャの実装(移行ステップ 1〜3)

対応する指示書: `docs/prompts/phase11.md`
設計の正本: `docs/architecture.md`(§0〜§12)、`docs/hostapi-next.md`
Phase 10 の実測前提: `docs/results/phase10.md`

生データ: `captures/phase11/`(.gitignore 対象)

---

## ステップ 0: 設計の穴埋め(2026-09-05、承認済み・コミット `0aa9833`)

指示書 §ステップ 0 の 2 点を仕様として確定させた。コードは書いていない。

### 0-1. `seq_write` の部分受理 — **(a) プレフィックス受理 + アプリが残りを保持**

決定内容と根拠は `docs/architecture.md` §11-9、契約本文は
`docs/hostapi-next.md` §5(`seq_write` / `seq_flush_after`)と §10(L2 実装イメージ)。

要件 1〜5 との突き合わせ(語彙が増えないかの検証):

| 要件 | 1 回の書き込み件数 | (a) プレフィックス受理 | (b) 全件受理 or 0 |
|---|---|---|---|
| 1 高精度メトロノーム | 1 拍 1〜2 件 | 飽和しない。語彙増なし | 語彙増なし |
| 2 楽曲メトロノーム | 小節頭 PC + クリック、数件 | 語彙増なし | 語彙増なし |
| 3 SMF インポート | 数十〜数百件(256 超もありうる) | 分割受理で必ず前進 | チャンク > 空き で 0 が続き前進しない。実用には「空き件数」照会が要る → **語彙増** |
| 4 2trk シーケンサ+録音 | パンチイン後の再供給が大量 | 前進する | 同上のリスク |
| 5 ドラムマシン | 4 声部 16 分 = 256 件で horizon 境界に張り付く | horizon が縮むだけ | 境界でゼロ進捗 → 無音 |

**(a) は 12 関数のままで 5 要件を通す。(b) は要件 3〜5 で `seq_space_available()` 相当の
追加を要求しうる**ため、(a) を採用した。

追加でユーザー指示により、**キューの未発火イベントを破棄する操作
(`transport_locate` / `seq_flush_after` / `transport_stop`)の後は、アプリが保持している
未受理分(PENDING)を破棄して `seq_filled_until()` から供給し直す**契約を §5 に明記し、
§10 のコードにも `drop_pending()` を追加した。

### 0-2. 未発火 note-off の破棄(鳴りっぱなし)— **v1 はアプリ責務のまま凍結**

`docs/architecture.md` §11-8 に記録。決め手は「`hostapi_midi_send` が L0 を通らない
生バイト経路として残る以上、**ホスト側のノート追跡は原理的に不完全**になる」こと。
推奨イディオム(`transport_stop()` → `hostapi_midi_send` で All Notes Off)は
追加 API なしで成立することを確認済み。再検討トリガは要件 4 の実装フェーズ。

---

## ステップ 1: L0 / L1 を native に実装(既存経路と並存)

### 追加・変更したファイル

| ファイル | 内容 |
|---|---|
| `src/components/seq/clock_authority.{hpp,cpp}` | **新規**。Clock Authority(§3)。I2S サンプルカウントをレートマスターにし、固定比換算・アンカー・レート切替時の継続規則・ppm 監視を 1 モジュールに閉じ込める |
| `src/components/seq/seq.{hpp,cpp}` | **新規**。L0(tick 順キュー・ディスパッチャ・ポート抽象)と L1(テンポ/拍子マップ・transport・ループ写像・MIDI クロックのグリッド生成) |
| `src/components/seq/CMakeLists.txt` | **新規** |
| `src/components/audio/audio.cpp` | I2S TX の `on_sent` を **enable より前に**登録(P10-1 の方式)。`ensure_i2s` / `reconfig_rate` から `clockauth::OnFormatChanged` を呼ぶ |
| `src/components/midi/midi.{hpp,cpp}` | `Midi_TxBytes()` を追加(Start/Stop の副作用を持たない生バイト送出)。既存の `Midi_Send` / クロック生成経路は不変 |
| `src/components/wasm_runtime/hostapi.cpp` | CLICK ポートの発音ハンドラを `seq::SetClickHandler` に登録。`hostapi_audio_reset` から `seq::Reset()` |
| `src/main/app_main.cpp` | `clockauth::Init()`(I2S 初期化前)、`seq::Init()`、起動時ヒープログ |

**既存経路は触っていない**: `Midi_NotifyBeatScheduled` / `Midi_NotifyBeatFired`、
現行クリックスケジューラ(`tone_schedule_impl`)、`hostapi_midi_send` の Start/Stop
副作用はそのまま。本ステップの L0/L1 は誰からも呼ばれない。

### CLICK ポートと既存音声経路の共存方法(指示書の事前調査項目)

調査結果:

- **発音の実際の入口は `audio::Play_Tone(freq, dur, level)`** → `Mp3Player::play_tone` →
  `xQueueSend(tone_queue_, msg, 0)`(深さ 4、満杯なら捨てる)。実書き込み(I2S)は
  専用の `click` タスク(優先度 18、静的 4KB スタック)が行う。**ノンブロッキングで
  どのコンテキストからでも呼べる**。既存の `click_timer_cb`(esp_timer タスク)も
  `tone_play_impl`(wasm タスク)も、すべてこの 1 点を通っている。
- したがって **L0 の CLICK ポートも `audio::Play_Tone()` を共有の出口にする**。
  音声側の追加配線はゼロで、I2S を二重に触ることもない。
- **トーンパレット**(`s_tones[8]`)は hostapi.cpp のアプリセッション状態なので、
  seq コンポーネントからは**コールバック(`seq::SetClickHandler`)経由で呼ばせる**。
  こうしないと `wasm_runtime` → `seq` → `wasm_runtime` の循環依存になる。
  パレット参照は既存の `tone_lookup()`(`s_click_mux` を 6 バイトのコピーの間だけ
  保持)をそのまま使い、**新しいロックも既存ロックの分割も行っていない**。
- ロック規律(§6)の充足: L0 のキュー/タイムラインは **seq.cpp 専用の `s_mux`**
  (portMUX)だけで守り、LVGL / FS / オーディオ書き込みのロックとは共有しない。
  ポートへの送出(`Midi_TxBytes` / `Play_Tone`)と `esp_timer` 操作は**必ずロックの外**で
  行う(ディスパッチャは臨界区間内で最大 16 件をローカル配列へ取り出し、
  ロックを抜けてから送出する)。

### 実装上の判断(設計に無かった細部)

- **UART TX の直列化は不要**と確認した。当初 `Midi_TxBytes` と `Midi_Send` を portMUX で
  囲もうとしたが、IDF の `uart_write_bytes` は内部で `tx_mux`(セマフォ)を取るため
  **portMUX の臨界区間から呼ぶのは不正**であり、かつその `tx_mux` によって
  **バイトの交錯は元々起こらない**。呼び出し側ではロックを取らない方針に戻し、
  理由を `midi.cpp` にコメントとして残した。
- **ディスパッチャの再アームは常に絶対時刻グリッド基準**
  (`host_us(tick) = 区間開始 µs + (tick − 区間開始 tick) × upq / 960`)。
  発火時刻からの相対加算は一切していない。系統オフセットの前倒し補正は
  `kFireAdvanceUs = 20`(P10-3 の 16〜26µs)。
- **イベントの取り出しと送出は esp_timer タスクだけの責務**にした。アプリタスク側の
  変更(`seq_write` / テンポ変更 / locate)は `rearm()` で**タイマの張り直しだけ**を行う。
  2 タスクが同時にキューを取り出して送出順が入れ替わる事故を構造的に排除するため。
- MIDI クロックは 40 tick グリッドから L1 が生成し、**キューを消費しない**。
  PLAYING 中はクロックグリッドが常に次の期限を供給するので、キューが空でも
  ディスパッチャは生き続ける(設計どおり)。

### 自己検査(`#ifdef PHASE11_L0_SELFTEST`)

起動時に L0 キューへ既知パターンを積んで検証する。**結果: PASS(0 failures)**
(`captures/phase11/monitor-step1-selftest.log`)。

| 検査項目 | 内容 | 結果 |
|---|---|---|
| tick 昇順 | 降順で 5 件投入 → 昇順に整列するか | ok |
| 安定順序 | 同一 tick 4 件の書き込み順が保たれるか | ok |
| 満杯時 | 256 件で満たしたあと 10 件 → 受理 0 | ok |
| 部分受理 | 252 件のあと 10 件 → **受理 4**(プレフィックス受理) | ok |
| flush_after | 100 件のうち tick>=60 を破棄 → 40 件、その後 filled_until = 59、全破棄 → 60 件 | ok |
| 端数バイト | 2 件 + 7 バイト → 受理 2 | ok |

検証後、コンパイル定義(`target_compile_definitions`)は削除済み。ソース側の
`#ifdef PHASE11_L0_SELFTEST` ブロックはステップ 3 完了時に削除する。

### メモリ(静的追加量)

| 項目 | サイズ |
|---|---|
| L0 キュー `s_queue[256]` | 4096 B |
| テンポマップ `s_tempo[32]` | 256 B |
| 拍子マップ `s_meter[32]` | 256 B |
| その他スカラ(区間・状態・ハンドル) | 数十 B |
| **恒久追加 合計** | **約 4.6 KB** |

起動直後の実測(ボードリセット直後、同一測定点で Phase 10 と比較):

| 測定点 | Phase 10 最終回帰 | Phase 11 ステップ 1 | 差分 |
|---|---|---|---|
| `Audio_Init` 前 free heap | 156372 | 151900 | **−4472** |
| `Audio_Init` 後 free heap | 109168 | 104696 | −4472 |
| `runtime ready` 時 free heap | 102780 | 98272 | −4508 |

差分は静的追加量(約 4.6KB)と一致しており、想定外の消費はない。

自己検査ビルドではさらに −4096 B(検査用の `static l0_event_t bulk[256]`)が乗るが、
これは検証専用でありコンパイル定義とともに無効化済み。

### 既存アプリ 7 種の回帰(実機、2026-09-05)— **合格**

生データ: `captures/phase11/monitor-step1-regression.log`。ループバック配線なしで実施。

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block | Phase 10 の largest block |
|---|---|---|---|---|---|
| demo | 54524 | 54524 | +0 | 31744 | 31744 |
| bars | 54524 | 54524 | +0 | 31744 | 31744 |
| touch_demo | 54524 | 54524 | +0 | 31744 | 31744 |
| mp3player | 54524 | 54480 | −44 | 31744 | 31744 |
| clicktest | 54480 | 54480 | +0 | 31744 | 31744 |
| metronome | 54480 | 54480 | +0 | 31744 | 31744 |
| midi_loopback | 54480 | 54480 | +0 | 31744 | 31744 |

- **`largest block` は全アプリ 31744 で Phase 10 最終回帰と完全一致。** 静的 4.6KB を
  追加しても最大連続ブロックは縮んでいない(WASM の linear memory 確保への影響なし)。
- free heap の水準が Phase 10 比 −4508B なのは静的追加量どおり。**リークは 0**
  (mp3player の −44B のみで、これは 9c 以前からの既知挙動。Phase 10 でも同値)。
- **WARN/ERROR 0 件**(起動時の `spi_flash: Detected size(16384k)...` のみ)。
  `MIDI RX: ring buffer full` も出ていない(ループバック配線なしのため)。

### 新たに判明した制約

- **アプリパーティションの残りが 1%(0x2f70 = 12144 B)しかない。**
  `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"` + `PARTITION_TABLE_SINGLE_APP` の設定に対し、
  **実機のフラッシュは 16MB** である(起動ログ:
  `spi_flash: Detected size(16384k) larger than the size in the binary image header(2048k)`)。
  ステップ 2(新 API 12 関数)とステップ 3(新 metronome の .wasm 埋め込み)で
  超過する可能性がある。設定変更はスコープ外なので、超過したら報告して停止する。
