# Phase 14: 旧経路の削除(移行ステップ 4b / 5)

## 目的

`docs/architecture.md` §10 の**移行ステップ 4b(旧クリック予約 API の削除)と
ステップ 5(`hostapi_midi_send` の Start/Stop 副作用の削除)**を完遂し、
**§1 に挙げた「テンポの二重管理」「毎拍の位相リセット」を生むコードをリポジトリから
物理的に消す**。Phase 13 で metronome が新 API へ移った結果、旧経路の利用者は
2 アプリだけになっており、いま消せば互換性負債を残さずに済む。

## 位置づけと前提

- セッション開始時に `docs/workflow.md`、`docs/lessons.md`、
  `docs/architecture.md`(§7 / §10 / §11-3 / §11-8)、`docs/hostapi.md`(§7 既存 API との関係)、
  `docs/results/phase13.md`(特に「次フェーズへの申し送り」)を通読すること。
- **移行ステップ 4(旧 API を L0 の薄いラッパに載せ替える中間ステップ)は行わない。**
  ステップ 4 は「旧アプリを動かしたまま内部だけ差し替える」ためのものだが、
  本フェーズで旧 API の利用者がゼロになるため、ラッパを作らずに削除(4b)へ直行する。
  この判断は `docs/architecture.md` §10 の移行表にも反映すること。
- **ABI 破壊を含む**が、`docs/architecture.md` §11-3 の「削除期限は ABI を対外的に
  確定版として公開する時点より前」という決定の範囲内である。リポジトリ内のアプリは
  すべて再ビルドできる。

### 現状の依存関係(Phase 14 開始時点、実測済み)

| 旧 API | 使っているアプリ | 本フェーズでの扱い |
|---|---|---|
| `hostapi_click_schedule` | clicktest / midi_loopback | **削除**(利用者をゼロにしてから) |
| `hostapi_tone_schedule` | **なし** | **削除** |
| `hostapi_midi_send` の Start/Stop 副作用 | midi_loopback のみ | **副作用だけ削除**(生バイト送出の API は残す) |
| `hostapi_play_click` / `hostapi_tone_play` / `hostapi_tone_define` | touch_demo / clicktest / metronome | **残す**(CLICK ポートがトーンパレットを使う) |
| `hostapi_midi_send`(生バイト送出) | midi_loopback / seq_smoke | **残す**(SysEx・即時 CC・All Notes Off 用) |

### 回帰の基準値(Phase 13 最終回帰)

- 対象 6 本 → **本フェーズで clicktest を削除して 5 本**になる。
- free heap は全アプリ **49136**(開始/終了の差分 +0)、**largest free block 31744**、
  許容外の WARN/ERROR 0 件。
- 削除によってフラッシュ・静的 RAM がどれだけ減るかを記録すること
  (`largest free block` が 31744 から動かないことの確認も含む)。

## ゲート(必須)

1. **各ステップ単位でビルド → 自動回帰 → コミット**する(`scripts/device-regress.sh`)。
   まとめて変更してから一括で回帰しない(回帰の切り分けが本フェーズを別立てにした理由)。
2. **ステップ 1 の受け入れは実測で行う**(下記の絶対値目標)。目標を満たさない場合は
   設計に合わせて解釈を曲げず、実測を報告して停止する。
3. Host API の**追加**はしない。削除と、削除に伴う宣言・ドキュメントの更新だけを行う。
   仕様どおりに書けない箇所が出たら、回避せず報告して停止する。
4. 検証専用コードは `#ifdef PHASE14_*_TEST` で囲み、検証後に削除する
   (`git grep PHASE14 -- src scripts wasm-apps tools` が空になること)。
5. **既存の回帰(削除後の 5 本)を壊さない。** 特に touch_demo / metronome が使う
   トーンパレット(`tone_define` / `tone_play` / `play_click`)は削除対象ではない。

## ステップ 1: midi_loopback を新 API へ移す

現状は `hostapi_midi_send(0xFA)` の**副作用**でホスト内部のクロック生成を起動し、
`hostapi_click_schedule` の毎 tick 再予約でテンポを間接的に伝えている
(= 9c の根本原因そのものの経路)。これを新 API に置き換える。

- START/STOP を `hostapi_transport_start()` / `hostapi_transport_stop()` に置換。
- テンポは `hostapi_tempomap_set_tempo(0, 500000)`(120bpm)を明示的に設定する。
  拍子は `hostapi_tempomap_set_meter(0, 4, 4)`。
- 可聴クリックが要るなら `seq_write(port=CLICK / OP_TONE)` で供給する
  (metronome と同じプレフィックス受理契約。`docs/hostapi.md` §5 / §10)。
  **不要なら供給しない**(本アプリの目的は受信統計なので、クリックなしの方が
  条件が綺麗になる。どちらにするか判断して報告すること)。
- `extern` 宣言から `hostapi_click_schedule` を削除する。`hostapi_midi_send` は
  **残してよい**(CC 等の生バイト送出に使う場合)。ただし **Start/Stop/Continue を
  単独バイトで送ってはならない**(ステップ 3 で副作用が消えるまでは二重送出になる)。
- **E1(ヒストグラム・ロバスト統計・外れ値・見かけ BPM 分布)は恒久機能として維持する。**
  これは 9c との比較系列であり、本フェーズ後は「PC なしで実機単体で回せる
  Phase 13 相当の測定」になる。

### 受け入れ(実測、ループバック配線)

実機 MIDI OUT → **自機 MIDI IN** のループバック配線に変更してもらい(ユーザーの物理作業)、
120bpm・4/4 で 5 分以上走らせて E1 の値を確認する。

| 項目 | 目標 | 根拠 |
|---|---|---|
| 外れ値(公称の 1.5 倍以上 / 0.5 倍以下) | **0 件** | Phase 13 の A1 / A3 |
| clocks / expected | **100%**(境界の ±1 発は許容) | 同上 |
| 見かけ BPM の分布 | **単峰**(115〜119 の第 2 峰がない) | 同上 |
| クロック間隔の平均 | **20833µs ± 10µs** | 同上 |

- `MIDI RX: ring buffer full` は「受信をドレインするアプリ」なので出ないはずである。
  出たら受信処理の取りこぼしを疑うこと(Phase 10 の既知挙動とは条件が違う)。
- Linux ホストでも同一 `.wasm` が動くこと(`app_init=0` / `app started` / `app stopped`、警告 0)。

## ステップ 2: clicktest を削除する

`hostapi_click_schedule` の唯一の存在理由だったアプリなので、API と一緒に消す。

- `wasm-apps/clicktest/` を削除(削除前にタグ `pre-old-api-removal` を打つ)。
- `src/components/wasm_runtime` の CMake の EMBED_FILES から外す。
- `scripts/device-regress.conf` の対象アプリから外す(6 本 → **5 本**)。
- `wasm-apps/README.md` のアプリ一覧、`docs/results/phase12.md` のカバレッジ表を更新する
  (**カバレッジ表は「どの Host API がどのアプリで踏まれるか」の根拠なので、
  削除後も穴が空かないことを確認して記録すること**)。

## ステップ 3: `hostapi_click_schedule` / `hostapi_tone_schedule` を削除する

利用者がゼロになったことを確認してから削除する。

- `shared/hostapi_defs.h`: `HOSTAPI_NATIVE_SYMBOLS` の 2 エントリと、該当する
  契約の記述(「予約はホスト側に常に 1 件のみ」「last_fired」等)を削除する。
- 実機 `src/components/wasm_runtime/hostapi.cpp`: native 実装、予約用の
  esp_timer(`click_timer_ensure` 等)、`last_fired` / 予約状態、
  `Midi_NotifyBeatScheduled` / `Midi_NotifyBeatFired` の**呼び出し側**を削除する。
- Linux `hosts/linux/hostapi_sdl.c`: `tone_schedule_impl` と `s_click_pending` 等の
  予約状態を削除する(即時発音 `tone_play_impl` は CLICK ポートが使うので残す)。
- `docs/hostapi.md` §7 の表を「非推奨化 → 削除」から**削除済み**に更新する。

## ステップ 4: `hostapi_midi_send` の Start/Stop 副作用と旧クロック生成器を削除する

- `src/components/midi/midi.cpp`: 単独バイトの 0xFA/0xFB/0xFC を見てクロック生成を
  開始/停止する分岐、`Midi_NotifyBeatScheduled` / `Midi_NotifyBeatFired`、
  クロック生成タイマとテンポ逆算の状態(`s_clock_running` / `s_last_target_ms` /
  `s_next_period_ms` 等)を削除する。`midi.hpp` の宣言も同様。
- `Midi_Reset()` が「アプリ破棄時にクロックを止める」ために依存していた部分を
  整理する(クロック生成は L0/L1 に一本化されるので、`seq::Reset()` 側で足りるはず)。
- Linux ホスト側に同等の副作用があれば同様に削除する。
- `shared/hostapi_defs.h` の `hostapi_midi_send` の記述から副作用の説明を削除し、
  **「System Realtime の送出は `transport_*` を使う」**と明記する。
- `docs/hostapi.md` §7 と `docs/architecture.md` §1 / §10 を更新する
  (§1 の「3 つの問題」のうち 1・2 が解消済みであることを記録する)。

## ステップ 5: 回帰と文書

- `./scripts/device-regress.sh --task phase14-regress` で **5 本**の回帰合格
  (free heap の差分 +0、largest block 31744、WARN/ERROR 0)。基準値からの
  free heap の変化(削除による静的分の増減)を記録する。
- Linux ホストで 5 本の起動確認(`app_init=0` / `app started` / `app stopped`、警告 0)。
- フラッシュ使用量の変化(削除でどれだけ減ったか)を記録する。
- `docs/architecture.md` §10 の移行表(ステップ 4 / 4b / 5 の完了と、
  **ステップ 4 を実施せずに 4b へ直行した理由**)、`docs/hostapi.md` §7、
  `docs/status.md`、`docs/lessons.md`、`docs/results/phase14.md` を更新する。
- 削除したコードの行数と、消えた概念(テンポ逆算・毎拍の位相リセット)を
  **Zenn 記事にそのまま引用できる形**で `docs/results/phase14.md` に残す。

## スコープ外

- 移行ステップ 6(内蔵音源のポート追加)
- PSRAM の本番反映(Phase 12 の「条件付き go」。別フェーズ)
- Song Position Pointer、STOPPED 中のクロック送出(§11-7 で確定済み)
- Phase 13 で未取得の測定(送信側 σ、テンポ切替点の数値確認)。**やるなら
  ステップ 1 の E1 で同時に採れるので、余裕があれば拾う**
- Host API の追加・シグネチャ変更

## 実行環境に関する指示

**実機ビルド、Linux ホスト用ビルド、flash、monitor、カメラ撮影、測定ツールの実行など、
シェルで実行するものはすべて herdr の pane を作成して実行すること。** pane 構成・
コマンド・タイムアウト値は `docs/workflow.md` に従い、pane 操作は `scripts/hpane.sh` を使う。

flash 前に `esp32-monitor` の docker コンテナがシリアルポートを保持していないか
`docker ps` で確認する(既知の教訓)。長時間の測定プローブは**ペインから切り離して
起動し、ユーザーに操作を依頼する直前に生存確認する**(Phase 13 の教訓)。

ステップ 1 の受け入れ測定では**配線変更(実機 MIDI OUT → 自機 MIDI IN)と
アプリ内の START/STOP 操作**にユーザーの物理作業が要る。依頼のタイミングを事前に伝え、
完了の返答を待ってから次へ進むこと。

## 完了条件

- midi_loopback が新 API だけで動作し、ステップ 1 の絶対値目標を実機で満たしている。
- clicktest が削除され、回帰対象が 5 本に更新されている(カバレッジ表も更新済み)。
- `hostapi_click_schedule` / `hostapi_tone_schedule` が
  `shared/hostapi_defs.h`・両ホストから削除され、`git grep` で残存がない。
- `hostapi_midi_send` の Start/Stop 副作用と `Midi_NotifyBeat*`、旧クロック生成器が
  削除され、`git grep NotifyBeat` で残存がない。
- 5 本の自動回帰に合格し、Linux ホストでも 5 本が起動・終了する。
- `docs/architecture.md` §1 / §10、`docs/hostapi.md` §7、`docs/status.md`、
  `docs/lessons.md`、`docs/results/phase14.md` が更新されている。
- 検証専用コード(`PHASE14_*_TEST`)が削除されている。
- `git status --porcelain` がクリーン、コミットはステップ単位(英語メッセージ)。

## 報告フォーマット

1. ステップ 1: midi_loopback の変更点(クリック供給の有無とその判断)、E1 の実測表
   (目標との対照)、Linux 動作、`.wasm` サイズ
2. ステップ 2: 削除した内容と、カバレッジ表に穴が空かないことの確認
3. ステップ 3: 削除した API・native 実装・状態変数の一覧、回帰結果
4. ステップ 4: 削除した副作用・関数・状態変数の一覧、回帰結果
5. ステップ 5: 回帰表(5 本)、フラッシュ/RAM の増減、文書更新の一覧
6. 申し送り: 次フェーズ(ステップ 6 / PSRAM / 内蔵音源)への引き継ぎ事項、
   実装時に困った点(あれば)
