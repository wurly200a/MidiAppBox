# Phase 17 実施記録 — テンポ / 拍子マップの寿命管理と小節境界での停止

- 指示書: `docs/prompts/phase17.md`
- 前提となる分析: `docs/results/phase16.md`(「Host API ギャップ分析」「Phase 17 への要求」)

---

## ステップ 0: 設計メモ(2026-09-13、**承認済み**)

本ステップではコードを変更していない。

> **承認結果(2026-09-13)**: 0-5 の 1〜5 をすべて承認(方式 B'、満杯時の畳み込みという挙動変更、
> Linux の C 単体テスト新設、`device-regress.conf` / `analyze.py` の変更、番号とシグネチャ)。

### 0-0. 前提の再確認(ソースで確認した箇所)

| # | 前提 | 確認箇所 |
|---|---|---|
| P1 | マップを消すのは `reset_state_locked` だけ。呼ぶのは `seqcore_init` と `seqcore_reset`(アプリ破棄時: 実機 `seq::Reset`、Linux `hostapi_seq.c:181`)。`transport_start` は `s_count = 0` でキューを空にするが、マップには触らない | `shared/seq_core.c` |
| P2 | `SEQCORE_TEMPO_MAX` / `SEQCORE_METER_MAX` = 32。`set_tempo` は PLAYING 中に過去の at_tick を -1 で拒否する。`set_meter` にはこのガードが無い | `seq_core.h` / `seq_core.c` |
| P3 | `bar_beat_locked` は拍子マップを song tick 0・小節 0 から走査する | `seq_core.c` |
| P4 | `seqcore_dispatch` は、同じ tick ではクロック → キューの順に取り出し、送出はロックの外で行う。ディスパッチャの文脈は、実機が esp_timer タスク(`ESP_TIMER_TASK`)、Linux が専用スレッド。どちらもディスパッチ中にコアのロックを保持していない | `seq_core.c` / `src/components/seq/seq.cpp` / `hosts/linux/hostapi_seq.c` |
| P5 | metronome の `apply_meter` は、PLAYING 中に **`set_meter(0, …)` を `locate(0)` より前に呼ぶ**(その瞬間の song tick は 0 より大きい)。`apply_tempo` は `locate(0)` → `set_tempo(0, …)`。seq_smoke は `set_loop` / 前方への `locate` / `stop` → `continue` を使う。midi_loopback は at_tick = 0 の設定と start / stop だけ | `wasm-apps/*/src/lib.rs` |
| P6 | 「STOPPED 中に at_tick = 0 で初期値を設定してから start」は、metronome / midi_loopback / seq_smoke がすべて使っている | 同上 |

着手前に分かった追加の事実:

- **F-a: Linux ホストには `seqcore_selftest` を走らせる経路が無い。** `SEQCORE_SELFTEST` を見ているのは実機の `app_main.cpp` / `seq.cpp` だけ。
- **F-b: `midi-clock-probe` は、再生区間(0xFA/0xFB〜0xFC)の外のクロックを黙って捨てる。**
  `analyze.py` の `clocks = [t for t in ev["clock"] if in_window(t) and span_of(t) is not None]`。
  期待数も時間から逆算した値。**「Stop の後のクロックが 0 発」「Stop までちょうど N 発」は現状の出力から読めない**。
- **F-c: `set_meter` に過去 tick のガードを足すと metronome が壊れる**(P5。`locate(0)` の前に at_tick = 0 へ書くため)。ガードは一律には足さない。

### 0-1. 方式の再評価と選択

| 案 | 内容 | 評価 |
|---|---|---|
| A | テンポ / 拍子 / 停止をすべて seq の制御オペコードにする | **不採用。** bar / beat の算出(P3)を増分方式に作り直す必要があり、テンポ区間の境界もキューの発火から作ることになる。ディスパッチャが大きく変わり、V4 のリスクが最大。さらに `seq_flush_after` / `locate` がテンポ変更まで消す意味論になり、紛らわしい |
| B(Phase 16 の推奨) | `tempomap_clear` + **通過済みエントリを常に剪定** + `OP_STOP` | **このままでは不採用。** 常時の剪定は、ループ・後方への `locate` で戻った先のエントリを消しうる。拍子の剪定は小節番号(P3)を狂わせる |
| **B'(採用案)** | `tempomap_clear`(STOPPED のみ)+ **満杯になったときだけ、通過済み区間を 1 件に畳む**(小節番号の起点を内部に保持)+ `OP_STOP` | **採用。** 畳み込みは「従来 -1 を返していた状況」でしか起きないので、**既存の成功経路の挙動は 1 ビットも変わらない** |
| C | `transport_start` がマップを消す | **不可。** P6 の契約を壊す |

### 0-2. 決定案の API と意味論

#### (1) `hostapi_tempomap_clear() -> 0/-1`(新規、シグネチャ `"()i"`)

- **STOPPED 中のみ有効。** PLAYING 中は何もせず -1(再生中に消すと、現在のテンポが既定値へ飛ぶため)。
- テンポマップ・拍子マップを空にし、ループを解除する。アプリ起動直後と同じ時間軸になる(有効値は既定の 500000 µs/4 分音符 = 120bpm、4/4)。小節番号の起点も 0 に戻す。
- transport の位置(STOPPED 中の locate 位置)とキューには触らない。
- 典型の使い方: `transport_stop` → (次の再生の前に)`tempomap_clear` → `set_tempo(0, …)` / `set_meter(0, …)` → `transport_start`。P6 の契約はそのまま成り立つ。

#### (2) 満杯時の畳み込み(既存関数の挙動変更。シグネチャ・ABI は不変)

- **発動条件**: PLAYING 中に `set_tempo` / `set_meter` が**新しい at_tick のエントリを足そうとして、マップが満杯**のとき。STOPPED 中は発動せず従来どおり -1(次の start は song tick 0 から始まり、過去の区間も使うため)。
- **床(floor)**: 現在の song tick。ループ設定中は `min(現在の song tick, loop_start)`(ループで戻る区間を消さない)。
- **規則**: `at_tick < floor` のエントリのうち**最後の 1 件だけを残し、それより前を削除する**。残した 1 件が、その区間の有効値を保つ。空きができれば挿入し、できなければ従来どおり -1。
- **小節番号(P3)**: 拍子マップを畳むときは、残すエントリの位置での小節番号を内部の起点(`s_meter_origin_tick` / `s_meter_origin_bar`、通常は 0 / 0)に保存する。`bar_beat_locked` はそこから走査する。**起点より後の bar / beat は、畳み込み前と同じ値になる。**
- **畳み込み後の制約**: 起点より前の at_tick への書き込みは -1。起点より前の song tick の bar / beat は保証しない。
- **意図的に受け入れること**: 畳み込み後に floor より前へ `locate` / `continue` すると、畳まれた区間のテンポ・拍子は「残した 1 件の値」になる。これは**従来 -1 で書けなかった状況でしか起きない**。
- 明示的な剪定関数(`tempomap_trim(before)`)を採らなかった理由: 語彙が 1 つ増え、アプリが floor を知る必要がある。自動の畳み込みは「失敗していた状況」を「成功する状況」に変えるだけで、既存の成功経路に触れない。

#### (3) `HOSTAPI_SEQ_OP_STOP = 2`(オペコード追加。ABI 非破壊)

書き方: `seq_write({tick: T, port: 0, status: HOSTAPI_SEQ_OP_STOP})`。**port は無視する**(transport 全体への操作なので)。

| 論点(指示書 b) | 決定 |
|---|---|
| 1. 停止 tick のクロック | **T のクロックは出さない。** T が 40 tick グリッド上なら、それは次の小節の 1 発目になるため。**T より前のクロックはすべて出る**(start からなら、Stop までちょうど ⌈T / 40⌉ 発) |
| 2. 停止後の位置 | song tick = song(T)、playback tick = T。`transport_continue` はそこから再開する。**遅れて発火した場合**(T が現在より過去)は、playback tick を `max(T, 次に出す予定だったクロックの tick)` にして単調性を保つ(song tick は song(T) のまま) |
| 3. キューの扱い | T より前のイベントは送出する。**T と同じ tick で OP_STOP より先に書かれたイベント(境界の note-off 等)は送出**し、**後に書かれたもの・T より後のイベントは破棄**する(`transport_stop` と同じ) |
| 送出順 | 同じ tick の先行イベント → 0xFC |
| 4. ロック規律 | 状態遷移(STOPPED、キュー破棄、停止位置)はディスパッチャの**ロックの中**で行い、送出は**ロックの外**。停止後は再アームしない(ディスパッチャが戻るだけ)。実機のワンショットは発火済み、Linux は `s_armed = false` 済みなので disarm は不要。他スレッドの `rearm` は `s_state` が PLAYING のときしかアームしないので競合しない |
| 5. 取り消し | 普通のキューイベントなので、`seq_flush_after(t ≤ T)` / `transport_locate` / `transport_stop` で消える |
| 6. `transport_stop` との関係 | 即時停止は従来どおり。停止後の状態は両者で同じ形になる(continue も同じ) |
| 通知 | 増やさない。アプリは `get_position` の `state` で停止を知る |

#### (4) 語彙の増加(指示書 f)

**関数 +1(`tempomap_clear`)、オペコード +1(`OP_STOP`)。**
`docs/hostapi.md` §6 の要件 1〜5 の検証は、暗黙に「1 回の再生」を前提にしていた。**アプリの中で再生を始め直す(曲を替えて再生する)ライフサイクルが抜けていた**ことが、Sequencer(要件 2 の実体)で初めて表に出た。
クリアをオペコードにできないのは、STOPPED 中はキューもディスパッチャも動かないため。境界停止はオペコードで表現でき、関数は増やさない。

#### (5) `shared/hostapi_defs.h` の差分案(指示書 g)

```c
enum {
    HOSTAPI_SEQ_OP_NONE = 0,
    HOSTAPI_SEQ_OP_TONE = 1, /* port=CLICK。param = トーンスロット (0..7) */
    HOSTAPI_SEQ_OP_STOP = 2, /* tick で transport を停止する。port は無視(Phase 17) */
};

    X(hostapi_tempomap_clear, "()i")                        \
```

- `shared/seq_core.h`: `int32_t seqcore_tempomap_clear(void);`
- 戻り値: `tempomap_clear` は 0 / -1(PLAYING)。`set_tempo` / `set_meter` は、畳み込んでも空かないとき、または畳み込み後に起点より前の at_tick を指定したときに -1(それ以外は従来どおり)。
- ABI 構造体(16 B / 32 B)は変えない。内部で増えるのは静的な 8 B(小節番号の起点)程度で、上限・キュー深さは変えない(ゲート 5)。

### 0-3. 既存 5 アプリへの影響(指示書 e)

| アプリ | 使っているもの | エントリ数の最大 | 影響 |
|---|---|---|---|
| touch_demo / mp3player | 音楽時間軸 API を使わない | 0 | なし |
| metronome | at_tick = 0 の上書き、`locate(0)` | tempo 1 / meter 1 | **なし**(満杯にならないので畳み込みは発動しない。起点は 0 のまま) |
| midi_loopback | at_tick = 0 の設定のみ | 1 / 1 | なし |
| seq_smoke | at_tick = 0 と小節頭 1 件、ループ、locate、continue | tempo 2〜3 / meter 1 | **なし**(新しい検査を足す) |

ディスパッチャの変更は、キュー取り出しループでの `status == OP_STOP` の比較 1 回だけ。定常の経路は変わらないが、V4 で metronome のクロックを測って確かめる。

### 0-4. 検証計画(指示書 h)

| 層 | 内容 | 合否 |
|---|---|---|
| **L-1: Linux の C 単体テスト(新規・提案)** | `hosts/linux/tests/seq_core_test.c`。偽の時計と送出を記録するフックで `shared/seq_core.c` をリンクし、ディスパッチを決定的に進める。`cmake --build` → `ctest` | clear / 満杯時の畳み込み(bar / beat / tempo が保たれる、ループの floor、起点より前は -1)/ OP_STOP(T のクロックが出ない、同 tick の順序、停止位置、continue、flush で取り消し)/ 既存の selftest 項目。**全件 PASS** |
| L-2: `seqcore_selftest` | 上と同等の軽量項目を足す(実機は `SEQCORE_SELFTEST` ビルドで確認) | 失敗 0 件 |
| **L-3: seq_smoke 拡張**(同じ `.wasm` を実機・Linux) | 既存 stage 7 の後に 3 ステージ追加(下記)。CHK は 8 → **12 bit**(CC#119/#120 の 14 bit に収まる)、`CHK_ALL = 0x0FFF` | 両ホストで PASS |
| V3 外部測定 | `midi-clock-probe` で seq_smoke の最終区間を記録(実機は UM-ONE、Linux は `--port MidiAppBox`) | 最終の 0xFA〜0xFC 区間のクロックが **ちょうど 288 発**(T = 3 小節 = 11520 tick)、0xFC の後は **0 発** |
| V4 | metronome の MIDI クロック(Phase 13 と同じ条件、実機、約 5.5 分) | 欠落 0 / 100.00% / 単峰 / 20833±10µs |
| 回帰 | `device-regress.sh` 5 本、Linux 5 本 | PASS / 警告 0 |

seq_smoke に追加するステージ:

- **stage 8(clear)**: STOPPED で `tempomap_clear` → `get_position` の `tempo_upq == 500000` → `CHK_CLEAR`
- **stage 9(V1 枯渇しない)**: 拍子 2/8 ↔ 3/8、テンポ 150000 ↔ 160000 µs/4 分音符を小節ごとに交互に先読みで予約し、**100 回以上**(上限の 3 倍)の変化を通過させる。毎 app_tick で `get_position` の bar / beat / tempo_upq を、アプリが計算した期待値と比べる。`set_*` が一度でも -1 なら失格 → `CHK_NOEXHAUST`。1 小節は平均約 190ms なので、所要は約 20 秒
- **stage 10(V2 再生の始め直し + V3 境界停止)**: stop → clear → 120bpm・4/4 で start。2 小節のあいだ `tempo_upq == 500000` かつ bar / beat が 4/4 どおり → `CHK_RESTART`。3 小節目の頭(T = 11520)に `OP_STOP` を予約し、停止後に `state == STOPPED` かつ song tick == T → `CHK_STOPAT`。最後に CC で結果を出す

### 0-5. 承認をお願いしたい事項

1. **方式 B'** と 0-2 の意味論。承認後に `docs/architecture.md` §11-10 に決定として記録する
2. **既存関数の挙動変更**(満杯時の畳み込み)。既存 5 本に影響しない根拠は 0-3
3. **Linux の C 単体テストの新設**(`hosts/linux/tests/`、CMake に target と `ctest` を追加)。F-a への対応で、OP_STOP の「T のクロックを出さない」を決定的に検査する手段でもある
4. **`scripts/` / `tools/` の変更**(ゲート 4)
   - `scripts/device-regress.conf`: `HOLD_OVERRIDE[seq_smoke]` を 20 → **60** 秒(seq_smoke が約 30 秒延びるため)
   - `tools/midi_clock_probe/analyze.py`: 「再生区間ごとのクロック数」と「停止中(区間外)のクロック数」の 2 行を出力に追加(F-b。既存の集計値は変えない)
5. 番号とシグネチャ: `HOSTAPI_SEQ_OP_STOP = 2`、`hostapi_tempomap_clear` = `"()i"`

---

## 決定した API と意味論

ステップ 0 の方式 B' のとおり(仕様は `docs/hostapi.md` §4 / §5、決定記録は `docs/architecture.md` §11-10)。

| 追加 / 変更 | 内容 |
|---|---|
| `hostapi_tempomap_clear()` `"()i"` | テンポ / 拍子マップとループを空にする。STOPPED のみ(PLAYING は -1) |
| 満杯時の畳み込み(`set_tempo` / `set_meter`) | PLAYING 中に満杯なら、floor より前を「最後の 1 件」に畳んでから挿入する。拍子は小節番号の起点を保持する |
| `HOSTAPI_SEQ_OP_STOP = 2` | 指定 tick で停止する。その tick のクロックは出さない。同 tick の先行イベント → 0xFC |

## 実装(共通コア / ホスト配線)

| ファイル | 変更 |
|---|---|
| `shared/seq_core.c` / `.h` | `seqcore_tempomap_clear`、畳み込み(`compact_tempo_locked` / `compact_meter_locked`、起点 `s_meter_origin_*` / `s_tempo_origin_tick`)、`bar_beat_locked` を起点から走査、ディスパッチャに `OP_STOP`、selftest に 2 項目追加 |
| `shared/hostapi_defs.h` | `HOSTAPI_SEQ_OP_STOP`、`X(hostapi_tempomap_clear, "()i")`、契約コメント |
| `src/components/wasm_runtime/hostapi.cpp` / `hosts/linux/hostapi_seq.{c,h}` | ネイティブラッパ(1 行ずつ) |
| `hosts/linux/tests/seq_core_test.c` / `hosts/linux/CMakeLists.txt` | **新設**。偽の時計でディスパッチを決定的に進める単体テスト 10 件、`ctest` |
| `wasm-apps/seq_smoke/` | stage 8〜12 と CHK 4 bit を追加(8 → 12 項目)。`.wasm` 5,029 → 6,431 B |
| `tools/midi_clock_probe/analyze.py` | 「再生区間ごとの 0xF8 数」「停止中の 0xF8 数」の 2 行 |
| `scripts/device-regress.conf` | `HOLD_OVERRIDE[seq_smoke]` 20 → 60 |

内部 RAM の静的な増加は起点 3 変数(12 B)。マップ上限・キュー深さは変えていない(ゲート 5)。
ファームウェアは 1,045,984 B(Phase 14)→ **1,045,776 B**(-208 B。Phase 15 の変更を含む比較)。

## selftest / seq_smoke の結果(実機・Linux)

| 検査 | Linux | 実機 |
|---|---|---|
| `seq_core_test`(`ctest`、既存 selftest を含む 10 件) | **10/10 PASS** | —(ホスト非依存の単体テスト) |
| `seqcore_selftest`(`SEQCORE_SELFTEST` ビルド) | PASS(`seq_core_test` の `existing_selftest`) | **`SELFTEST PASS (0 failures)`**(追加 2 項目を含む。`captures/phase17/monitor-selftest.log`) |
| seq_smoke(同じ `.wasm`) | **chk 4095**(CC#119 = 127 / CC#120 = 31、12 項目すべて合格) | **chk 4095**(同左) |

`seq_core_test` の内訳: `existing_selftest` / `clear` / `restart_after_clear` / `tempo_compaction` / `meter_compaction_keeps_bar_numbers` / `loop_protects_entries` / `stop_on_boundary` / `stop_is_cancelled_by_flush` / `late_stop_keeps_playback_tick_monotonic` / `metronome_style_usage_unchanged`。
ログ: `captures/phase17/seq_core_test.log`、`aseqdump-linux.log`、`aseqdump-device.log`。

## V1 / V2 / V3 / V4

- **V1(枯渇しない)— 合格(両ホスト)**: seq_smoke stage 9 で、テンポ・拍子それぞれ **101 エントリ**(上限 32 の 3 倍超)を先読みで予約し、一度も -1 が返らず、境界から離れた位置の bar / beat / tempo_upq がすべて期待どおり(`CHK_NOEXHAUST`)。`seq_core_test` でもテンポ・拍子それぞれ 100 回、小節番号がずれないことを確認した。
- **V2(再生の始め直し)— 合格(両ホスト)**: stage 10 で clear → 120bpm・4/4 で再開し、2 小節半のあいだ upq = 500000・4/4 の bar / beat(`CHK_RESTART`)。V1 で畳み込みが起きた後なので、**clear が効いていなければ at_tick=0 への設定自体が -1 になる**条件で確かめている。`seq_core_test` では「clear しないと前回の予約が効く」ことも再現した。
- **V3(境界停止)— 合格(両ホスト)**: 最終区間(start → `OP_STOP` @ 11520)の 0xF8 がちょうど **288 発**、0xFC の後は **0 発**。

  | ホスト | 記録器 | 再生区間ごとの 0xF8 | 停止中の 0xF8 | 0xFA / 0xFB / 0xFC |
  |---|---|---|---|---|
  | Linux | `midi-clock-probe --port MidiAppBox` | 604 / 198 / 3002 / **288** | **0** | 2 / 1 / 4 |
  | 実機 | `aseqdump -p UM-ONE` | 605 / 195 / 3016 / **288** | **0** | 3 / 1 / 4 |
  | 実機 | `midi-clock-probe --port UM-ONE` | 605 / 195 / 3016 / **288** | **0** | 3 / 1 / 4 |

  最終区間以外の値がホストで違うのは、それらの区間の停止を app_tick(100ms 周期)から行っているため(stage 3 / 6 / 9)。
  Linux の probe で 0xFA が 2 回(本来 3 回)なのは、ホストの ALSA ポートが現れてから probe が接続するまでの間に、
  seq_smoke が `app_init` で最初の 0xFA を送っていたため。probe は開始を取り逃がした区間を最初のクロックから始めるので、区間ごとの数と停止中の数は正しい。

- **V4(metronome の MIDI クロック)— 合格**: 最終ファームウェア(selftest 定義なし、起動と 5 アプリ列挙を確認)で、
  実機 MIDI OUT → UM-ONE → PC、120bpm・4/4・アイドル 385 秒(ユーザーが START / STOP を操作)。

  | 項目 | 目標 | Phase 17 | 参考: Phase 15 T-1 | 参考: Phase 13 A1 |
  |---|---|---|---|---|
  | 0xF8 / 期待数 | 100.00% | **18480 / 18480.6 = 100.00%** | 100.00% | 100.00% |
  | 外れ値(≥1.5x / ≤0.5x) | 0 件 | **0 件** | 0 件 | 0 件 |
  | 見かけ BPM | 単峰 | **単峰**(118.75〜121.26) | 単峰 | 単峰 |
  | 平均間隔 | 20833±10µs | **20832.8µs** | 20832.9µs | 20832.8µs |
  | 停止中の 0xF8 | —(新しい行) | 0 | — | — |
  | 受信側 σ(参考) | — | 424.4µs | 417.5µs | 428.8µs |

  受信側 σ とヒストグラムの形(20333µs 未満に約 3,000 件)は Phase 13 / 15 と同じで、UM-ONE の USB 経由で広がる受信側の分布である
  (`docs/workflow.md` §3.5「受信側の σ で送信精度を判定しない」)。ディスパッチャへの `OP_STOP` 判定の追加による悪化は見られない。
  ログ: `captures/phase17/v4-metronome.{csv,md}`、`monitor-v4.log`。

## 回帰

- **実機(`device-regress.sh --task phase17`)— PASS**: 5 本とも free_int / PSRAM の差分 +0、largest_int 57,344(不変)、許容外の WARN/ERROR 0 件。free_int の水準は 105,880(Phase 15)→ 105,856(-24 B)。
- **Linux** — 5 本とも `app_init=0` / `app started` / `app stopped`、警告 0、残留プロセスなし。
  1 回目の touch_demo は、**実機の回帰と同時に走らせたため**、実機の seq_smoke が UM-ONE へ流したクロックを Linux ホストの MIDI IN が受け、ドレインしない touch_demo で `midi: RX ring buffer full` が 4,081 行出た。構成依存の挙動(Phase 10 最終回帰と同じ)で、実機の回帰が終わってから取り直して警告 0 を確認した。

## 仕様からの逸脱

| # | 内容 | 理由 |
|---|---|---|
| 1 | 遅れて発火した `OP_STOP` の停止後 playback tick を、設計メモの「次に出す予定だったクロックの tick」ではなく **「最後に出したクロックの直後」**(`max(T, next_clock - 40 + 1)`)にした | 設計メモの式では、グリッドの外にある T(例: 100)で時間どおりに止めても playback tick が次のグリッド(120)に進んでしまう。単調性を保つのに必要なのは「出したクロックより前に戻らない」ことだけ |
| 2 | テンポの畳み込みの floor に「現在のテンポ区間の開始」を加えた(拍子は現在の song tick のまま) | ディスパッチャが区間境界をまだ処理していない間に、その境界のエントリを消さないため。拍子は区間と無関係なので、floor を現在位置にしないと、テンポが一定の曲では拍子マップが畳めない |
| 3 | `set_meter` の満杯時、挿入位置を件数で補正せずに探し直す | 畳み込み内部で floor を取り直すので、厳密に一致させるため |

## Phase 18 への申し送り

- **再生を始め直すたびに `stop → tempomap_clear → at_tick=0 の初期値 → start`。** clear しないと前回の未来の予約が効き、畳み込み後は at_tick=0 へ書けない。
- **小節境界で止めるときは `OP_STOP` を境界 tick に予約する。** `transport_start` はキューを空にするので、start の **後** に積むこと。停止はアプリから `get_position().state` で知る。
- テンポ / 拍子の予約は先読み範囲(1〜2 小節)に留める。畳み込みは「先読み範囲で 32 件を超えない」ことが前提。
- `seqcore`(Rust)の `BarEvents.stop` は、OP_STOP の予約に写す。

## トラブル

- **一時定義の撤去で `src/CMakeLists.txt` を壊しかけた。** 実機 selftest のために足した `SEQCORE_SELFTEST` の行を Edit で消したところ、
  前後の改行も消えて `LV_COLOR_16_SWAP` の行と `project()` の行が 1 行に連結された。**ビルドに使う前に `git diff` で気づき、
  `git checkout -- src/CMakeLists.txt` でコミット済みの内容へ戻した**(差分 0 を確認してから通常ファームを再ビルド)。
  最終ファームウェアは selftest 定義なしで起動し、`SELFTEST` の行が出ないことも確認した。
- 実機の回帰と Linux の回帰を同時に走らせ、Linux 側に `midi: RX ring buffer full` が出た(「回帰」節)。取り直して警告 0。

## 完了条件

| 完了条件 | 状況 |
|---|---|
| ステップ 0 の設計メモが承認され、`architecture.md` §11 に決定記録がある | **達成**(§11-10) |
| 新 API が両ホストに実装され、`seqcore_selftest` と seq_smoke が両ホストで全 PASS | **達成** |
| V1〜V3 が合格し、測定値が本ファイルにある | **達成** |
| metronome が Phase 13 の絶対値目標を満たす(V4) | **達成** |
| `device-regress.sh` 5 本 PASS、Linux 5 本の起動・終了で警告 0 | **達成** |
| `hostapi.md` / `hostapi_defs.h` / spec §6 / `lessons.md` / `status.md` の更新 | **達成** |

## 残課題

- **`docs/roadmap.md` の Phase 17 の行はまだ planned のまま。** CLAUDE.md の規則どおり、Phase 18 の指示書を書くときに実績(本ファイル)を反映する。
- metronome は、Phase 13 の回避策(`locate(0)` → at_tick=0 の上書き)のまま。マップを消費しないので畳み込みも clear も不要で、書き換える必要は無い(指示書のスコープ外)。
- 畳み込み後に floor より前へ `locate` / `continue` したときのテンポ・拍子・bar / beat は保証しない(意図的なトレードオフ、§11-10)。この使い方をするアプリが現れたら、再検討のきっかけにする。
- 測定の運用: ALSA の記録器は、測定対象を起動する **前** に接続を済ませること。seq_smoke は `app_init` で即 0xFA を送るので、`--wait` で待ち受けても最初の 0xFA を取り逃がすことがある(集計は区間の数え方で吸収される)。
