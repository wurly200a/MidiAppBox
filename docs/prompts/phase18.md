# Phase 18: Session 画面と単体再生(Sequencer app の初回 `.wasm`)

- 契約日: 2026-09-13
- 参照: `docs/apps/sequencer/spec.md`(§3〜§5、§7 の Q5 / Q6)、`docs/results/phase16.md`(seqcore と Phase 18 への申し送り)、`docs/results/phase17.md`(「Phase 18 への申し送り」)、`docs/hostapi.md`、`docs/roadmap.md`
- 結果報告先: `docs/results/phase18.md`

## 目的

Sequencer app の最初の `.wasm` を作る。**Session を単体で再生できる**ところまでを、実機と Linux の両方で動かす。

- spec §5.2 のうち Phase 18 担当の 3 画面(**Menu / Session 一覧 / Session 画面**)
- Session 画面の再生: Play / Stop、トグル `1` / 矢印(spec §4.1 の 4 通り、再生中の変更は次の小節境界から)、メトロノームクリック(1 拍目アクセント)、再生中小節の点滅
- MIDI: Start / Stop と 24ppqn クロック(ホストが生成)。自然終了は小節境界ちょうどで止める

曲構造の解釈はすべて `wasm-apps/seqcore/`(Phase 16)を使い、アプリ側に同じロジックを書き直さない。

## 前提(再調査不要。ただし着手時にソースで確認すること)

- **P1: seqcore(Phase 16)**: `Transport::play_session` / `set_session_toggles` / `queue(JumpBar)` / `advance_bar` / `advance_beat` / `stop`。境界で送るべきものを `BarEvents` で返す。`Transport` は `Copy` なので、複製して進めれば先読みできる。`Bank::new()` は全ビット 0(`static` に置けば .bss)。
- **P2: Host API(Phase 11 / 17)**:
  - 再生を始め直すたびに `transport_stop → tempomap_clear → at_tick=0 の初期値 → transport_start`
  - 小節境界で止めるときは、`transport_start` の **後** に `seq_write` で `HOSTAPI_SEQ_OP_STOP` を境界 tick に積む(`transport_start` はキューを空にする)
  - クリックは `seq_write(port=CLICK, OP_TONE)` + `tone_define`(metronome と同じ)
  - テンポ / 拍子の予約は先読み範囲(1〜2 小節)に留める(満杯時の畳み込みはその前提)
- **P3: 描画 API の制約**(`shared/hostapi_defs.h`、`src/components/wasm_runtime/hostapi.cpp`、`hosts/linux/hostapi_sdl.c`)
  - **draw_text / fill_rect は座標 (x, y) をキーにした retained モデルで、スロットは各 16。**
  - **一度使った座標のスロットは、アプリが破棄されるまで解放されない**(空文字列でも解放されない)。**画面を消す API も、移動・削除の API も無い。**
  - 溢れると `no free slot` の警告を出して無視される。文字色は白固定。
  - したがって、1 つのアプリの中で複数の画面を切り替えるには、**すべての画面で同じ座標の組を使い回す**必要がある(mp3player の 6 行リスト + スクロールボタンと同じ考え方)。
  - **重なり順がホストで違う**: 実機(LVGL)は後から作ったオブジェクトが上、Linux は全 rect を描いてから全 text を描く。
- **P4: 入力**: `hostapi_poll_event` のタッチ DOWN / UP(シングルタッチ、`time_ms` 付き)だけ。長押しは時刻差でアプリが判定する(metronome の長押し連打が実例)。
- **P5: メモリ**: linear memory は PSRAM 上にあり、大きさは問題にならない(Phase 15)。効く制約は次の 2 つ(`docs/results/phase16.md`「メモリ見積もり」)。
  - wasm スタックは 8KB。`Bank`(9,842 B)を値で扱わない
  - 非ゼロ初期値の `static` は `.wasm` を太らせる。**いずれかの `.wasm` が 16KB を超えたら roadmap U-6(Strategy B)に触れる**ので、サイズを記録する
- **P6: 検証の制約**(`docs/workflow.md` §1-8、`docs/lessons.md`)
  - 実機のタッチ操作はユーザーに依頼する
  - Linux ホストのクリック自動化は信頼できず、**Linux の画面キャプチャもこの環境では取れない**(roadmap U-12)。画面の動作動画は実機(カメラ)だけで撮る
- **P7: 回帰**: 現在の回帰対象は 5 本。アプリを足すには、ファームへの埋め込み(`src/components/wasm_runtime/CMakeLists.txt` の `EMBED_FILES`)、ランチャーの seed(`launcher.cpp`)、`scripts/device-regress.conf` の `APPS` の 3 箇所が要る。
- **P8: roadmap U-2**: PSRAM のリーク監視が「1 回の起動→停止の差分」までしかない。Bank を持つ Sequencer を回帰に加える前に、**同じアプリを N 回繰り返したときの非減少判定**を入れる計画になっている。

## ゲート(必須)

1. **ステップ 0(設計メモ)の報告 → 承認** を経てから実装に入る。
2. **Host API / ABI は変更しない。** 描画 API の制約(P3)で仕様の画面が作れないと判断した場合は、実装せずに「何が足りないか・最小の拡張案」を報告して止まる(承認されたら別フェーズ、またはスコープ変更の追記)。
3. `scripts/` / `tools/` / `docs/workflow.md` の変更は提案 → 承認。**`CLAUDE.md` の回帰対象の記述(現在 5 本)を変える場合もユーザーの承認を得る。**
4. seqcore の公開 API を変える場合は、変更点と理由を設計メモに書き、既存の 34 テストを通したうえで行う。

## スコープ

### 含む

#### ステップ 0: 設計メモ(実装なし・承認ゲート)

`docs/results/phase18.md` のステップ 0 節に、次をすべて書く。

- **a. 画面設計と描画スロットの予算表**
  - 3 画面(Menu / Session 一覧 / Session 画面)の共通レイアウト。どの座標を何に使い回すか
  - text / rect それぞれ 16 以下に収まることを表で示す(画面ごとの使用状況と、使わないセルの消し方も)
  - spec §5.3 の描き分け(**再生中マーカーの点滅**と**選択カーソル**)を、座標キーの retained モデルでどう表現するか
  - 重なり順の差(P3)を踏まないための作成順の規則
  - 16 小節の Bar 一覧を何行ずつ見せるか、スクロールの操作
- **b. 操作設計**
  - Play / Stop、トグル `1` / 矢印、Bar の選択(タップ)、戻る、Session 一覧 → Session 画面の遷移
  - **Q5(ジャンプ操作の UI)の決定**。spec §5.3「タップ = 選択のみ、ジャンプは別操作で次の小節境界にキュー」に沿って、長押しかボタンかを決める
  - Session scope の**テンポの変え方**。spec に UI の規定が無い。Transport の現在テンポをどの画面でどう変えるか
  - **Q6(カウントインを v1 に含めるか)の決定**
  - **Session scope で PC を送るか**の決定(spec §4.2 は Song scope のみ。`docs/results/phase16.md` 残課題)
- **c. 時間軸の設計(L2)**
  - seqcore の `Transport` と、ホストの playback tick / song tick の対応のさせ方(song tick は単調なタイムラインとして使い、`locate` / `set_loop` は使わない = Phase 16 の前提)
  - 先読み(何小節先まで、`advance_bar` の複製での peek の使い方)
  - 小節ごとに積むもの: クリック、テンポ / 拍子の予約、自然終了時の `OP_STOP`
  - **トグル変更・ジャンプの締め切り**: 境界まで 1 app_tick 未満で変更された場合の扱い(`docs/results/phase16.md` 残課題)。先に積んだ分の取り消し(`seq_flush_after` と、テンポ / 拍子の同 at_tick 上書き)
  - 画面遷移しても再生は止まらないこと(spec §4)。アプリを抜けたら停止する
  - **この計画ロジックのうちホスト非依存にできる部分を seqcore に置くか**(置けば Linux の `cargo test` で検査できる)
- **d. デモデータ**: 永続化は Phase 20 なので、`app_init` で組み立てる組み込みの Bank(Session 数・小節数・拍子の上書きを含む例)。ゼロ初期化の `static` に置く
- **e. 検証計画**(下記ステップ 3 の具体化)
- **f. U-2 の設計**: `device-regress.sh` に「同じアプリを N 回起動→停止して PSRAM / internal が単調に減らないこと」を足す方法(N、判定式、所要時間)

#### ステップ 1: seqcore の拡張(ステップ 0 で必要と決めた場合のみ)

- 計画ロジックを seqcore に置くと決めたら、その部分を足し、`cargo test --features std` と wasm32(no_std)ビルドを通す。

#### ステップ 2: `wasm-apps/sequencer/` の実装

- 3 画面、再生、トグル、点滅、クリック、MIDI(Start / Stop / クロック、自然終了の `OP_STOP`)。
- `.wasm` をビルドしてコミットする(既存アプリと同じ手順)。**サイズを記録する**(16KB を超えたら U-6 に触れることを results に書く)。
- ファームへの埋め込み・ランチャーの seed・回帰対象への追加(P7)。

#### ステップ 3: 検証

- **Linux**: 起動 → 画面遷移を伴わない範囲で動作 → ESC 終了。`app started` / `app stopped`、**`no free slot` を含む警告 0**、残留プロセスなし。
- **実機(ユーザー操作 + カメラ)**: ステップ 0 の e で決めた操作手順を、録画しながらユーザーに依頼する。最低限、次を映像とログで確認する。
  - 3 画面の遷移と、戻ったときに前の画面の残骸が残らないこと
  - トグル 4 通り(全小節 1 回 / 全小節繰り返し / 選択小節 1 回 / 選択小節繰り返し)
  - 再生中にトグルを変えると、次の小節境界から効くこと
  - 再生中小節の点滅と選択カーソルが別物として見えること、拍子を上書きした小節のクリック数とアクセント
  - 再生中に Session 一覧へ戻っても再生が止まらないこと
- **MIDI の測定(実機、`midi-clock-probe`)**: 「全小節 1 回」の再生で、Start から Stop までのクロック数が Session の長さ(拍子の上書きを含む)と**ちょうど一致**し、Stop の後は 0 発であること。
- **回帰**: `device-regress.sh` で **6 本**(既存 5 本 + sequencer)が PASS。U-2 の反復判定を入れた場合はその結果も。Linux も 6 本で警告 0。
- **seqcore**: `cargo test --features std` が全件 PASS(既存 34 + 追加分)。

#### ステップ 4: 文書化

- `docs/results/phase18.md`、`docs/apps/sequencer/spec.md`(Q5 / Q6 の確定、Session scope の PC、画面の実装に合わせた §5 の更新)、`wasm-apps/README.md`(アプリ一覧)、`docs/lessons.md`、`docs/status.md`。
- 次フェーズの指示書を書くときに `docs/roadmap.md` を更新する。

### 含まない

- Song 一覧 / Chapter 一覧、arrangement 再生、Session 境界の PC 送信、SL MK3 との end-to-end(Phase 19)
- 永続化、編集(追加 / 削除 / 並べ替え)(Phase 20)
- Host API / ABI の変更(ゲート 2)
- Song / Chapter 画面へのトグル横展開(Q7、Phase 20 以降)

## 進め方

1. セッション開始時に `CLAUDE.md`・`docs/workflow.md`(通読)・`docs/lessons.md`・本書の参照先を読み、`docs/workflow.md` §3.0 の環境確認を行う。
2. **ステップ 0 報告 → 承認 → ステップ 1〜4。** 各ステップの結果は `docs/results/phase18.md` に記録する。
3. 確認は Linux → 実機の順。シェル操作はすべて `scripts/hpane.sh` 経由。実機の回帰と Linux の回帰を**同時に走らせない**(`docs/results/phase17.md`: 実機の MIDI 出力を Linux ホストが受けて警告が出る)。
4. 仕様・本書に無い判断をした場合は、results の「仕様からの逸脱」に必ず記録する。

## 完了条件

- [ ] ステップ 0 の設計メモ(描画スロットの予算表、Q5 / Q6 / Session scope の PC の決定を含む)が承認されている
- [ ] `wasm-apps/sequencer/` の `.wasm` がコミットされ、サイズが記録されている
- [ ] 実機で 3 画面・トグル 4 通り・次の小節境界での反映・点滅と選択の描き分けが、録画(`captures/phase18/`)とログで確認されている
- [ ] 「全小節 1 回」の Start〜Stop のクロック数が Session の長さと一致し、Stop 後は 0 発(実機)
- [ ] `device-regress.sh` 6 本 PASS(U-2 を入れた場合はその判定も)、Linux 6 本で警告 0(`no free slot` を含む)
- [ ] seqcore の `cargo test` が全件 PASS
- [ ] spec(Q5 / Q6 / §5)・`wasm-apps/README.md`・`docs/lessons.md`・`docs/status.md` が更新されている

## `docs/results/phase18.md` の雛形

```markdown
# Phase 18 実施記録 — Session 画面と単体再生

## ステップ 0: 設計メモ(a〜f)
## 実装(seqcore の追加 / sequencer app / 埋め込み)
## .wasm サイズとメモリ
## 検証(Linux / 実機の操作と録画 / MIDI 測定)
## 回帰(U-2 を含む)
## 仕様からの逸脱・spec.md への反映
## Phase 19 への申し送り
## 残課題
```

## 追記(スコープ変更)

- (日付: 内容)
