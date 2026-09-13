# Phase 17: テンポ / 拍子マップの寿命管理と小節境界での停止(Host API 追加)

- 契約日: 2026-09-13
- 参照: `docs/results/phase16.md`(「Host API ギャップ分析」「Phase 17 への要求」)、`docs/hostapi.md`、`docs/architecture.md` §6 / §9 / §11、`docs/apps/sequencer/spec.md` §6(H8 / H9)、`docs/roadmap.md`
- 結果報告先: `docs/results/phase17.md`

## 目的

Sequencer app(Phase 18)が既存の音楽時間軸 API だけでは満たせない 2 点を、Host API に足す。

- **要求 1(必須、H8)**: 同じアプリの中で再生を何度始め直しても、また長時間再生しても、テンポ / 拍子の予約が正しく効くこと。
- **要求 2(推奨、H9)**: 小節境界の playback tick を指定して停止できること。MIDI Stop とクロックの停止が同じ tick で起きること。

実機と Linux の両ホストに同時に入れる。ロジックは両ホスト共通の `shared/seq_core.c` に置く(`docs/lessons.md`「同じロジックを二重に書かない」)。

**本 Phase は Host API / ABI を変更する。ステップ 0 の承認ゲートを必ず通すこと。**

## 前提(Phase 16 で確認済み。着手時にソースで再確認すること)

- **P1**: テンポ / 拍子マップを消すのは `seqcore_reset`(アプリ破棄時、`reset_state_locked`)だけ。**`transport_start` は消さない。**
- **P2**: マップの上限は `SEQCORE_TEMPO_MAX` / `SEQCORE_METER_MAX` = 32 件。エントリを削除する語彙は無い。`set_tempo` は PLAYING 中に現在位置より過去の at_tick を -1 で拒否する。`set_meter` には同様のガードが無い。
- **P3**: `get_position` の bar / beat は、拍子マップを song tick 0 から走査して算出している(`bar_beat_locked`)。
- **P4**: `transport_start` は L0 のキューを空にする。ディスパッチャは同じ tick ではクロック → キューイベントの順に送出し、ポート送出はロックの外で行う(`seqcore_dispatch` / `port_dispatch`)。
- **P5**: 既存アプリの使い方:
  - metronome: 再生中のテンポ / 拍子変更で `transport_locate(0)` し、at_tick = 0 のエントリを上書きする
  - seq_smoke: `set_loop` / `locate` / `continue` を検査項目として使う
  - midi_loopback: at_tick = 0 の設定と start / stop のみ
- **P6**: 既存の契約「STOPPED 中に `set_tempo(0, …)` / `set_meter(0, …)` で初期値を設定してから `transport_start`」(`docs/hostapi.md` §4 / §6 要件 1)。**これを壊さない。**
- **P7**: Sequencer は song tick を単調な演奏タイムラインとして使い、`transport_locate` / `tempomap_set_loop` を使わない(`docs/results/phase16.md`)。ただし Host API は Sequencer 専用ではないので、ループ・locate と共存できること。
- **P8**: 0xFC は `seq_write(port=DIN_OUT)` でも送れるが、`hostapi_defs.h` の規約(リアルタイムバイトは `transport_*`)に反するので、要求 2 の解にしない。
- 回帰対象は 5 本(touch_demo / mp3player / metronome / midi_loopback / seq_smoke)。実機の回帰は `scripts/device-regress.sh` を既定とする。

## ゲート(必須)

1. **ステップ 0(設計メモ)の報告 → 承認** を経てから実装に入る。承認された決定は `docs/architecture.md` §11 に記録として残す(11-10 以降)。
2. **既存シンボルのシグネチャは変えない。** 既存 API の挙動を変える方式(例: 通過済みエントリの自動剪定)を採る場合は、既存 5 アプリへの影響をソースを根拠に示して承認を得る。
3. **ABI 構造体(`hostapi_seq_event_t` 16 B / `hostapi_position_t` 32 B)のサイズとレイアウトは変えない。** 拡張は enum 値・新シンボルの追加で行う。
4. `scripts/` / `tools/` / `docs/workflow.md` の変更は提案 → 承認。
5. **`SEQCORE_TEMPO_MAX` / `SEQCORE_METER_MAX` / `SEQCORE_QUEUE_DEPTH` を増やして解決しない。** これらは internal RAM の静的 BSS(`docs/architecture.md` §9)。どうしても増やすなら、根拠と internal への影響の実測を添えて提案する。

## スコープ

### 含む

#### ステップ 0: 設計メモ(実装なし・承認ゲート)

`docs/results/phase16.md` の方式案 A(seq 制御オペコード)/ B(`tempomap_clear` + 通過済みエントリの剪定 + `OP_STOP`、Phase 16 の推奨)/ C(`transport_start` でマップを消す、既存契約違反で不可)を、前提 P1〜P8 に照らして再評価し、1 つを選ぶ(新しい案でもよい)。
採る案について、以下すべてに答えること。

- **a. 要求 1**
  - 再生を始め直すたびにマップをリセットする手段。P6 を壊さないこと
  - 長時間再生でエントリが枯渇しない仕組み。剪定するなら、どのエントリをいつ消すかの規則
- **b. 要求 2(境界停止の API の形と意味論)**
  1. 停止 tick のクロックを出すか。境界 tick は 40 tick グリッド上にあり、出すと次の小節の 1 発目になる
  2. 停止後の song 位置(`transport_continue` の開始点)と playback tick
  3. キューに残っている、停止 tick と同じ tick / それ以降のイベントの扱い
  4. ロック規律(`architecture.md` §6)。状態変更はロックの内、送出はロックの外。ディスパッチャ(タイマ文脈)から停止してよいか、`timer_disarm` をどこで呼ぶか
  5. 予約した停止の取り消し手段(`seq_flush_after` で消えるか、など)
  6. 既存の `transport_stop` との関係(即時停止は従来どおり)
- **c.** `get_position` の bar / beat が、剪定・リセット後も正しく出続けること(P3)
- **d.** ループ再生中・`locate` で後ろへ戻ったときに、必要なエントリを失わない規則(P5 の metronome / seq_smoke)
- **e.** 既存 5 アプリへの影響。変わらない場合もソースを根拠に示す
- **f.** `docs/hostapi.md` §0「API 語彙が増えないことが層の切り方の検証」との関係。関数を増やすなら、その理由
- **g.** 追加・変更するシンボル、シグネチャ、戻り値 / エラー値、`shared/hostapi_defs.h` の差分案
- **h.** 下記ステップ 3 の検証 V1〜V3 の具体化(何を、どのアプリで、何を合否にするか)

#### ステップ 1: 共通コアの実装

- `shared/seq_core.c` / `seq_core.h` に実装する。
- `seqcore_selftest` に新しい挙動の検査を足す(リセット、剪定、境界停止、既存項目の非退行)。**両ホストで実行して失敗 0 件を確認する**(Linux で selftest を走らせる手段が無ければ、ビルド時定義で有効にする形で足す)。

#### ステップ 2: ホストへの配線と仕様書

- 新シンボルのネイティブラッパ(`src/components/wasm_runtime/hostapi.cpp`、`hosts/linux/hostapi_seq.c`)と `HOSTAPI_NATIVE_SYMBOLS` への追加。
- `docs/hostapi.md` と `shared/hostapi_defs.h` のコメントを、決定した意味論で更新する。

#### ステップ 3: 検証

- **seq_smoke の拡張**: 新 API の自動検査を CHK ビットとして追加する。CC#119 / #120 で外へ出す判定値は 14 bit に収めること。**同じ `.wasm` で実機・Linux とも全項目 PASS。** 回帰の保持時間(`HOLD_OVERRIDE[seq_smoke]=20` 秒)に収まる形にする。
- **V1(枯渇しない)**: テンポ / 拍子の変化を、上限 32 件の 3 倍以上の回数だけ通過させる(検査のためにテンポを速くし、短い拍子を使ってよい)。一度も -1 が返らず、`get_position` の bar / beat / tempo_upq が毎回期待どおりであること。
- **V2(再生の始め直し)**: 同じアプリの中で、テンポ・拍子の予約が異なる再生を 2 回続けて行う。2 回目に 1 回目の予約が混ざらないこと。
- **V3(境界停止)**: 予約した停止 tick で MIDI Stop が出て、**それ以降のクロックが 0 発**、Stop までのクロック数が期待値と一致すること。`scripts/midi-clock-probe.sh` で、実機(UM-ONE 経由)と Linux(`--port MidiAppBox`)の両方で測る。停止後のクロック数を集計する機能がツールに無ければ、追加を提案する(ゲート 4)。
- **V4(非退行)**: L1 / ディスパッチャに手が入るので、metronome の MIDI クロックが Phase 13 と同じ絶対値目標を満たすこと: **欠落 0 / clocks÷expected 100.00% / 見かけ BPM 単峰 / 平均間隔 20833±10µs**(120bpm・4/4、アイドル約 5.5 分、実機)。

#### ステップ 4: 回帰と文書化

- `scripts/device-regress.sh` で 5 本 PASS。Linux ホストでも 5 本の起動・終了で警告 0。
- `docs/architecture.md` §11(決定記録)、`docs/hostapi.md`、`shared/hostapi_defs.h`、`docs/apps/sequencer/spec.md` §6(H8 / H9 を実装済みに)、`docs/lessons.md`、`docs/status.md`、`docs/results/phase17.md` を更新する。
- 次フェーズの指示書を書くときに、`docs/roadmap.md` を更新する。

### 含まない

- Sequencer app 本体、`wasm-apps/seqcore/` の変更(Phase 18 への申し送りは results に書く)
- 拍 / 小節イベントの通知 API、境界同期の `locate`(Phase 16 で不要と判定)
- `hostapi_midi_recv` の線速補正(roadmap U-4)、Song Position Pointer(U-13)
- キュー深さ・マップ上限の拡張(ゲート 5)
- metronome の書き換え(新 API を使うように直すのは任意。やるなら提案 → 承認)

## 進め方

1. セッション開始時に `CLAUDE.md`・`docs/workflow.md`(通読)・`docs/lessons.md`・本書の参照先を読み、`docs/workflow.md` §3.0 の環境確認を行う。
2. **ステップ 0 報告 → 承認 → ステップ 1〜4。** 各ステップの結果は `docs/results/phase17.md` に記録する。
3. 確認は Linux → 実機の順(`docs/workflow.md` §2)。シェル操作はすべて `scripts/hpane.sh` 経由。
4. 仕様・本書に無い判断をした場合は、results の「仕様からの逸脱」に必ず記録する。

## 完了条件

- [ ] ステップ 0 の設計メモが承認され、`docs/architecture.md` §11 に決定記録がある
- [ ] 新 API が両ホストに実装され、`seqcore_selftest` と seq_smoke が両ホストで全 PASS
- [ ] V1〜V3 が合格し、測定値が `docs/results/phase17.md` にある
- [ ] metronome が Phase 13 の絶対値目標を満たす(V4)
- [ ] `device-regress.sh` 5 本 PASS、Linux 5 本の起動・終了で警告 0
- [ ] `docs/hostapi.md` / `shared/hostapi_defs.h` / spec §6 / `docs/lessons.md` / `docs/status.md` が更新されている

## `docs/results/phase17.md` の雛形

```markdown
# Phase 17 実施記録 — テンポ / 拍子マップの寿命管理と小節境界での停止

## ステップ 0: 設計メモ(a〜h)
## 決定した API と意味論
## 実装(共通コア / ホスト配線)
## selftest / seq_smoke の結果(実機・Linux)
## V1 / V2 / V3 / V4
## 回帰
## 仕様からの逸脱
## Phase 18 への申し送り
## 残課題
```

## 追記(スコープ変更)

- (日付: 内容)
