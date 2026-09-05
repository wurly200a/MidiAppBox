# Phase 11 実施記録 — 新アーキテクチャの実装(移行ステップ 1〜3)

対応する指示書: `docs/prompts/phase11.md`
設計の正本: `docs/architecture.md`(§0〜§12)、`docs/hostapi.md`
Phase 10 の実測前提: `docs/results/phase10.md`

生データ: `captures/phase11/`(.gitignore 対象)

---

## ステップ 0: 設計の穴埋め(2026-09-05、承認済み・コミット `0aa9833`)

指示書 §ステップ 0 の 2 点を仕様として確定させた。コードは書いていない。

### 0-1. `seq_write` の部分受理 — **(a) プレフィックス受理 + アプリが残りを保持**

決定内容と根拠は `docs/architecture.md` §11-9、契約本文は
`docs/hostapi.md` §5(`seq_write` / `seq_flush_after`)と §10(L2 実装イメージ)。

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

### 新たに判明した制約(ステップ 1 時点)

- **アプリパーティションの残りが 1%(0x2f70 = 12144 B)しかない。**
  `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"` + `PARTITION_TABLE_SINGLE_APP` の設定に対し、
  **実機のフラッシュは 16MB** である(起動ログ:
  `spi_flash: Detected size(16384k) larger than the size in the binary image header(2048k)`)。
  ステップ 2(新 API 12 関数)とステップ 3(新 metronome の .wasm 埋め込み)で
  超過する可能性がある。設定変更はスコープ外なので、超過したら報告して停止する。

---

## ステップ 2: Host API 追加(実機 + Linux ホスト同時)

### 採用した方式 — **L0/L1 のロジックを両ホストで共有する**

Linux 側に L0/L1 を書き下ろすと実装が二重化し、「同一 `.wasm` が実機と Linux で
同じ挙動」がコードレベルでは保証されない。そこで**ステップ 1 で実機に書いた
L0/L1 のロジックを移植可能な C に切り出し、両ホストが同じソースを使う**形に
リファクタした。

| ファイル | 役割 |
|---|---|
| `shared/seq_core.{h,c}` | **新規**。L0/L1 の全ロジック(キュー・ディスパッチャ・テンポ/拍子マップ・transport・ループ写像・クロックのグリッド生成・ポート抽象)。OS API を一切呼ばない |
| `src/components/seq/seq.cpp` | 実機のプラットフォーム束ね(portMUX / esp_timer ワンショット / `midi::Midi_TxBytes` / トーンパレット) |
| `hosts/linux/hostapi_seq.{h,c}` | **新規**。Linux のプラットフォーム束ね + 12 関数の native 実装 |

プラットフォーム依存は `seqcore_hooks_t`(`now_us` / `lock` / `unlock` / `arm` /
`disarm` / `send_midi` / `click`)の 7 個のフックに外出しした。これは
Phase A(ブラウザホスト)への移植点でもあり、**Clock Authority の抽象が
実機都合に引きずられていない**ことの実地確認になっている。

`shared/hostapi_defs.h` には `docs/hostapi.md` §8 のコード片を取り込んだ
(`HOSTAPI_PPQN`、transport 状態 / ポート / オペコードの enum、
`hostapi_seq_event_t`(16B)、`hostapi_position_t`(32B)、
`HOSTAPI_NATIVE_SYMBOLS` への 12 関数の追記)。サイズは `seq_core.c` の
`_Static_assert` で凍結している。**既存 API のシグネチャ・挙動は不変。**

### Linux の Clock Authority

| 項目 | 実機 | Linux |
|---|---|---|
| レートマスター | I2S TX の `on_sent` 累計サンプル | SDL オーディオコールバックの累計フレーム |
| 時刻源(v1) | `esp_timer`(固定比。P10-2 で −0.00ppm) | `hostapi_midi.c` の単調増加 µs = `hostapi_midi_recv` と同一時基 |
| 逐次推定 | 不要(固定比) | 不要(v1)。ppm 推定は診断として実装済み |
| ディスパッチ | `esp_timer` ワンショット 1 本 | 専用スレッド + `pthread_cond_timedwait`(CLOCK_MONOTONIC) |

Linux で `SDL_AddTimer` を使わなかったのは **ms 分解能しかなく 20833µs の
クロックグリッドを表現できない**ため。時刻源を `hostapi_midi.c` の時計に
揃えたのは、`time_us_to_tick` が `hostapi_midi_recv` の打刻をそのまま変換
できるようにするため。

### 検証用アプリ `wasm-apps/seq_smoke/`

タップなしで一巡するよう自動化した(Linux ホストの UI クリック自動化は
信頼できないため。`docs/lessons.md`)。動作: 起動と同時に
`tempomap_set_tempo(0, 500000)` / `set_meter(0,4,4)` / `transport_start` →
毎拍 CLICK + DIN_OUT の Note On/Off を先読み供給(**プレフィックス受理契約に
従い、受理されなかった残りを保持して再送する L2 ループ**)→ 4 小節目の頭で
`tempomap_set_tempo(BAR*4, 333333)`(180bpm)→ 8 小節で `transport_stop`。
実機用に自機ループバック受信の集計表示も持つ。

### 検証結果

**Linux ホスト**(生データ: `captures/phase11/linux_seq_smoke.log`。
送出バイトを一時トレースして統計化。トレースは検証後に削除済み):

| 項目 | 実測 | 期待 |
|---|---|---|
| 0xFA / 0xFC | 1 / 1 | 1 / 1 |
| 0xF8 総数 | 770 | 768(8 小節 × 96)+ 停止判定の粒度分 |
| クロック間隔 @120bpm | mean **20833.1µs** / σ 33.6(n=384) | 20833.3µs |
| クロック間隔 @180bpm | mean **13888.9µs** / σ 32.3(n=385) | 13888.9µs |
| Note On / Off | 33 / 32 | 対応どおり |
| 対応しない Note Off | **0** | 0 |

**実機**(同一 `.wasm`。ループバック配線 OUT→IN、seq_smoke の受信集計表示):

| 項目 | 実測 | 判定 |
|---|---|---|
| 受信クロック数 | **774** | 期待下限 768 以上。**欠落なし** |
| 0xFA / 0xFC | **1 / 1** | ok |
| Note On / Off | **33 / 32** | Linux と完全一致 |
| クロック間隔 min/avg/max | 12667 / **17338** / 22052 µs | 加重期待平均 17356µs と一致(−18µs) |

`min`/`max` が公称から ±1220µs 振れているのは **P10-5 で定量化済みの受信打刻
バッチング**である(`rx_task` が 1 回の UART イベント内の複数バイトに同一時刻を
付ける)。±1220µs ≒ 4 バイト × 320µs で、**クロックバイトが 3 バイトのノート
メッセージと同一イベントに載ったときの見かけのずれ**にあたる。送信側は
Linux の送出時刻で σ33µs と安定しており、**送信ジッタではない**。

> ステップ 3 の前後比較(metronome、クロックのみでノートを流さない条件)では
> このバッチングは発生しない(9c / P10-3 の測定条件と同じ)。ここで見えた
> ±1.2ms を回帰と取り違えないこと。

### 12 関数の検証表(最終)— **全 12 関数を実機・Linux 双方で検証済み**

`wasm-apps/seq_smoke/` を自動一巡するテストアプリに拡張し、**同一の `.wasm`** を
実機と Linux ホストで走らせた。アプリが自分で合否を判定し、8 個のチェック結果を
ビットで持つ(画面表示 + CC#119/#120 での外部出力)。

| 関数 | 確認方法 | 実機 | Linux |
|---|---|---|---|
| `transport_start` | 0xFA 送出 + 24ppqn グリッド開始 | ✔ | ✔ |
| `transport_stop` | 0xFC 送出 + クロック停止(`stp`) | ✔ | ✔ |
| `transport_continue` | 0xFB 送出 + 停止点から継続(`con`) | ✔ | ✔ |
| `transport_locate` | song tick が移動し playback tick は戻らない(`loc`) | ✔ | ✔ |
| `transport_get_position` | bar / beat / tick / song_tick / upq / host_us | ✔ | ✔ |
| `tempomap_set_tempo` | PLAYING 中の 120→180、キュー積み直しなし(`tmp`) | ✔ | ✔ |
| `tempomap_set_meter` | 4/4 の bar / beat が正しい | ✔ | ✔ |
| `tempomap_set_loop` | song tick が巻き戻り playback tick は単調増加(`lop`) | ✔ | ✔ |
| `seq_write` | CLICK 発音 + DIN_OUT の Note On/Off、プレフィックス受理 | ✔ | ✔ |
| `seq_flush_after` | 未発火分が実際に減る(`flu`)+ 自己検査 | ✔ | ✔ |
| `seq_filled_until` | 供給ループの停止条件、flush 前後の比較 | ✔ | ✔ |
| `time_us_to_tick` | PLAYING 中は tick に一致(`u2t`)/ STOPPED は -1(`u2s`) | ✔ | ✔ |

判定結果:

| ホスト | 結果 |
|---|---|
| 実機 | `PASS chk 255 st7`(8 項目すべて合格) |
| Linux | `B0 77 7F` + `B0 78 01` = chk **255**(同上)、FA×1 / FB×1 / FC×2 |

`time_us_to_tick` の検証に **`transport_get_position` が返す `host_us` を使う**のが
要点である。これなら MIDI IN の受信に依存せず、同じ判定を両ホストで行える
(受信打刻を使う実運用の経路とも同一時基)。

Note On 34 / Note Off 31 の差 3 は、`transport_locate` / `transport_stop`(2 回)/
`seq_flush_after` でキューを捨てたぶんの**未発火 note-off** であり、
`architecture.md` §11-8 に記録した「v1 は All Notes Off をアプリ責務とする」挙動
そのものである(仕様どおり)。

#### 検証の途中で判明した、自分の検証条件の誤り

初回の実機実行は `chk 127`(`flu` のみ不合格)だった。原因は**ホスト側ではなく
検証条件**で、`seq_flush_after` の後に `seq_filled_until() <= now_tick` を要求して
いたこと。キューが空になると `seq_filled_until` は「現在の playback tick」を返し、
その値は刻々進むため、判定した時点では `now_tick`(同 tick の先頭で取得した値)を
既に超えていることがある。Linux ではたまたま通り、実機で落ちた。
**「flush の前後で `filled_until` が減ったか」**に直して両ホストで合格。

### 仕様の食い違いを 1 件発見(実装は MIDI の慣行に合わせた)

`docs/hostapi.md` §3 の記述が 2 か所で矛盾している:

- `transport_start`: 「song tick 0 から再生を開始する。playback tick も 0 にリセット」
- `transport_locate`: 「STOPPED 中: 次の **start**/continue の開始位置になる」

locate 後に start すると 0 に戻るのか locate 位置から始まるのかが決まらない。
実装は **MIDI の慣行(Start = 先頭から / Continue = 現在位置から)**に合わせ、
`transport_start` は常に 0 から、`transport_locate` は `transport_continue` の
開始位置を決める、とした。**2026-09-06、仕様側を修正済み**(§3 の
`transport_locate` から「start/」を落とし、理由を併記)。

### 仕様書の整理(2026-09-06 実施済み)

`shared/hostapi_defs.h` に取り込んだ後も、**§2(tick の 2 座標)・§6(アプリ要件
突き合わせ表)・§9(time_us_to_tick の精度)は設計判断の根拠として価値がある**
一方、§8 のコード片は重複になった。実施内容:

- `docs/hostapi-next.md` → **`docs/hostapi.md`** に改名(`git mv`。「案」ではなく
  現行仕様)。参照していた他ドキュメント・ヘッダのリンクも更新した。
- §8 のコード片を削除し、**「宣言の実体は `shared/hostapi_defs.h`」への参照表**に
  置き換えた(型・enum・シンボル表を二重管理しないため)。
- 冒頭の「未承認ドラフト」表記を外し、実装済み・検証済みであること、および
  変更時は `hostapi_defs.h` が正であることを明記した。
- §2 / §6 / §9 は設計判断の根拠として残した。

### 既存アプリ 7 種の回帰

**Linux ホスト(2026-09-05)— 合格**(生データ: `captures/phase11/linuxreg/*.log`):

| アプリ | 終了 | app_init | 警告/エラー行 |
|---|---|---|---|
| demo / bars / touch_demo / mp3player / metronome / clicktest / midi_loopback | 全て clean | 全て 0 | 全て **0** |

`app started` / `app stopped` が全アプリで各 1 回、残留プロセスなし。

**実機(2026-09-05)— 合格**(生データ:
`captures/phase11/monitor-step2-regression.log`。ループバック配線を外して実施):

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block |
|---|---|---|---|---|
| demo | 54052 | 54052 | +0 | 31744 |
| bars | 54052 | 54052 | +0 | 31744 |
| touch_demo | 54052 | 54052 | +0 | 31744 |
| mp3player | 54052 | 54008 | −44 | 31744 |
| clicktest | 54008 | 54008 | +0 | 31744 |
| metronome | 54008 | 54008 | +0 | 31744 |
| midi_loopback | 54008 | 54008 | +0 | 31744 |

- **`largest block` は全アプリ 31744**。Phase 10 最終回帰・ステップ 1 と完全一致で、
  ステップ 2 の追加(12 関数 + `seq_core` への切り出し)でも最大連続ブロックは不変。
- free heap の水準はステップ 1 比 −472B。新 native 12 個の登録テーブル
  (`NativeSymbol` 配列)と追加コード分で、**静的追加として想定内**。
- **リークは 0**(mp3player の −44B のみ。9c 以前からの既知挙動)。
- **WARN/ERROR 0 件**(起動時の `spi_flash: Detected size(16384k)...` のみ)。
  配線を外したので `MIDI RX: ring buffer full` も出ていない。

起動直後の基準値: `Audio_Init: free heap 151428 -> 104224`、
`runtime ready ... free heap 97800`、`heap after seq init: largest block 57344`。

### 検証専用コードの後片付け

- **Linux ホストの送出トレース**(`midi_output_bytes` の `#ifdef PHASE11_SEQ_TRACE`)は
  削除済み。ALSA が使えない環境ではホストが元々ログ出力へフォールバックするため、
  トレースなしでも送出バイト列は確認できる。
- **L0 の自己検査**は削除せず、`PHASE11_L0_SELFTEST` → **`SEQCORE_SELFTEST`** に
  改名して**恒久の opt-in テスト**として残した。フェーズ限定の検証足場ではなく
  L0 キューの単体テストであり、呼ばなければ内部の作業バッファごとリンカに
  落ちる(実測でも static 追加は生じていない)。ビルド時に `SEQCORE_SELFTEST` を
  定義すると起動時に走る。
- `git grep PHASE11` で残存なしを確認済み。

### 新たに判明した制約(ステップ 2 時点)

- **アプリパーティションの残りが 0x14d0 = 5328 B** になった(seq_smoke 埋め込み後、
  バイナリ 0xfeb30 / 0x100000)。ステップ 3 で新 metronome の `.wasm`(約 3KB)を
  埋め込むと残り約 2KB。**超過したら停止して報告する。**
  実機フラッシュは 16MB あるので `CONFIG_ESPTOOLPY_FLASHSIZE` の変更で解消できるが、
  スコープ外なので勝手には変えない。
- **ステップ 3 の前後比較は指示書の記述どおりには実施できない。** 実機は WASM
  アプリを同時に 1 つしか動かせないため、「metronome を動かしながら
  `midi_loopback` の E1 で測る」が成立しない(9c では `midi_loopback` 自身が
  送信も受信も行っていた)。測定方式をユーザーと相談してから進める。候補:
  1. 測定側を Linux ホスト + UM-ONE にする(実機 MIDI OUT → UM-ONE → PC で集計)
  2. metronome2 に一時的な計測表示(`PHASE11_*_TEST`)を入れる
  3. `midi_loopback` の送信を新 API に切り替えて同一アプリ内で A/B する

---

## 最終回帰(2026-09-06)

seq_smoke を 12 関数の自動検証アプリへ拡張し、検証専用コードを片付けた最終ビルドで実施。
実機バイナリは `0xfeff0`(アプリパーティション 0x100000 に対し残り 0x1010 = 4112 B)。

### 実機 — **合格**(生データ: `captures/phase11/monitor-final.log`)

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block |
|---|---|---|---|---|
| demo | 54052 | 54052 | +0 | 31744 |
| bars | 54052 | 54052 | +0 | 31744 |
| touch_demo | 54052 | 54052 | +0 | 31744 |
| mp3player | 54052 | 54008 | −44 | 31744 |
| clicktest | 54008 | 54008 | +0 | 31744 |
| metronome | 54008 | 54008 | +0 | 31744 |
| midi_loopback | 54008 | 54008 | +0 | 31744 |
| (参考) seq_smoke | 54052 | 54052 | +0 | 31744 |

- **`largest block` は全アプリ 31744**。Phase 10 最終回帰・ステップ 1・ステップ 2 と
  完全一致で、本フェーズを通して最大連続ブロックは一度も縮んでいない。
- **リークは 0**(mp3player の −44B のみ。9c 以前からの既知挙動)。
  12 関数を叩き倒す seq_smoke でも **+0**。
- **WARN/ERROR 0 件**(起動時の `spi_flash: Detected size(16384k)...` のみ)。
- 起動時基準値: `Audio_Init: 151428 -> 104224`、`runtime ready ... free heap 97800`。

### Linux ホスト — 合格

全 7 アプリが `app_init=0` / `app started` / `app stopped`、残留プロセスなし。

各アプリのログに出る「警告/エラー行 2」は、いずれも
`open /dev/snd/seq failed: Permission denied` と
`midi: snd_seq_open failed (falling back to log-only)` の 2 行で、
**リモートデスクトップ経由で実行しているための環境要因**である
(ホストは仕様どおりログ出力へフォールバックして動作を継続する)。アプリ由来の
警告・エラーは 0 件。

**1 回目の実行で demo / clicktest の 2 つが SDL のウィンドウ生成前で止まり
`KILLED` になったが、再実行では全アプリ正常だった。**ログはどちらも ALSA の 2 行で
終わっており `font:` 行(SDL 初期化後)にも到達していないことから、アプリ側では
なく実行環境(リモートデスクトップ経由の X)の一時的な失敗と判断する。

> なお ALSA が使えない状態では、ホストが MIDI バイトを 1 本ずつ stderr へ書く
> フォールバック経路に入る。**この状態で採ったクロック間隔の統計は測定として
> 無効**である(σ が 800µs 級に膨らむ)。タイミングの正式値は ALSA が生きていた
> 2026-09-05 の測定(120bpm: mean 20833.1µs / σ33.6、180bpm: mean 13888.9µs /
> σ32.3)を用いること。

## Phase 12 への申し送り

1. **アプリパーティションの残りが 4112 B(0%)。** 実機フラッシュは 16MB あるが
   `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"` + `PARTITION_TABLE_SINGLE_APP` の設定になって
   いる。Phase 12 は metronome を**上書き**で書き直す方針なので大きくは増えない
   見込みだが、余裕がないことは事実。必要になったら
   (a) seq_smoke の埋め込みを外す、(b) フラッシュサイズ設定を見直す、のどちらかを
   選ぶ。設定変更はスコープ外として本フェーズでは触っていない。
2. **前後比較ではなく絶対値目標**で判定する(指示書の追記どおり)。実機は WASM
   アプリを同時に 1 つしか動かせないため、測定は「実機 MIDI OUT → UM-ONE → PC」で
   採るのが 9c のプロトコルに最も近い。**ただし現状 Linux 側で ALSA が使えない**
   ので、測定前に ALSA が使える状態(ローカルセッション等)を用意する必要がある。
3. **既存経路はまだ生きている。** `hostapi_midi_send` の Start/Stop 副作用と
   `Midi_NotifyBeatScheduled` / `Midi_NotifyBeatFired` によるテンポ逆算は
   ステップ 5 まで残る。新 API と併用すると**クロックが二重に出る**ので、
   新 metronome は `hostapi_midi_send` で Start/Stop を送らないこと。
4. **`SEQCORE_SELFTEST`** を定義してビルドすると L0 キューの自己検査が起動時に走る
   (恒久の opt-in テスト)。回帰時に使える。
