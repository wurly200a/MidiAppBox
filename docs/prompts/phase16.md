# Phase 16: Sequencer コア（host 非依存）と SL MK3 PC 挙動確認

- 契約日: 2026-09-13
- 参照: `docs/apps/sequencer/spec.md`（仕様）、`docs/roadmap.md`（位置づけ）
- 結果報告先: `docs/results/phase16.md`

## 目的

Sequencer app の中核ロジックを **Host API に依存しない `no_std` crate** として実装し、Linux 上の単体テストで正しさを固める。
同時に、後続 Phase の前提となる 3 つの未決事項（SL MK3 の PC 挙動、メモリ上限、Host API ギャップ）に答えを出す。

この Phase では **画面を作らない**、**Host API を変更しない**。

## 前提（再調査不要）

- Phase 15 完了。WASM 線形メモリは PSRAM 上に配置される
- `app_tick` のジッタ上限は 5.1ms。24ppqn クロック生成とメトロノーム click は native 側の既存スケジューラが担う
- MIDI Start / Stop / Continue の送信、`hostapi_midi_recv` は Phase 9a で存在する
- Session はグローバル、Chapter は Song 内定義で arrangement から参照、テンポは Song 側の TempoTrigger（`spec.md` §2–3）
- 既存 app の crate 構成・ビルド手順は `docs/workflow.md` と既存 `apps/` に従う（本 Phase で読んで確認すること。推測しない）

## スコープ

### 含む

1. `seqcore` crate（仮称、既存の命名規則に合わせてよい）
   - `spec.md` §3 のデータモデル（`heapless` または固定配列。依存 crate の追加は最小限）
   - §3.2 の解決規則: `effective_tempo(song, pos) -> u16`、`effective_meter(session, bar) -> TimeSig`
   - §4 の Transport 状態機械: `Scope::Song` / `Scope::Session{single_bar, repeat}`、小節境界での前進、終端処理、`QueuedAction`
   - 境界イベントの出力: Transport を 1 小節進めたとき「PC 送信が必要か」「テンポ / 拍子が変わったか」を **値として返す**（MIDI 送信自体はしない）
   - `#![no_std]` でビルドでき、`std` feature 付きで `cargo test` が通ること
2. 単体テスト（最低限）
   - テンポ解決: トリガー無し / 先頭 / 途中 / 途中 Session から開始
   - 拍子解決: Session 既定 / 小節上書き
   - Transport: Song 全体を 1 回再生して停止、Session 4 通りのトグル、再生中のトグル変更が次の小節境界で効くこと、`QueuedAction::Jump`
   - 同一 Session を 2 つの Chapter から参照した arrangement で PC が Session 境界ごとに出ること
3. SL MK3 実機実験（ユーザーが SL MK3 側を操作。Claude Code は送信側 app と手順書を用意）
   - 既存 app（または最小の使い捨て app）から PC を送り、SL MK3 が **即時** に Session を切り替えるか **パターン末尾** で切り替えるかを確認
   - 受信 MIDI チャンネル、Bank Select の要否を確認
   - 結果から `PC_LEAD_TICKS` の暫定値を決める
4. メモリ見積もり
   - `spec.md` §3 の上限定数での `Bank` サイズを算出し、`.wasm` 線形メモリ内に収まるか、上限を下げるべきかを判定
5. Host API ギャップ分析
   - `spec.md` §6 の H1–H6 について、既存 Host API（実際の定義ファイルを読む）で足りるか、拡張が要るかを表にする
   - 要る場合は Phase 17 の prompt に書くべき要求（関数の粒度、境界同期の方式案 2 つ以上）をまとめる。**実装はしない**

### 含まない

- LVGL 画面、`.wasm` としての sequencer app 本体（Phase 18）
- Host API の追加・変更（Phase 17、承認ゲート）
- 永続化、編集機能（Phase 20）
- MIDI Clock 精度問題の解析（別トラック。ただし実験 3 の副産物として観察があれば results に残す）

## 進め方

1. **調査報告 → 承認 → 実装** の順を守る。着手時にまず以下を報告して承認を待つ
   - 既存 `apps/` の crate 構成と、`seqcore` をどこに置くか（`apps/sequencer/seqcore` か workspace 直下か）
   - `heapless` 等の依存追加の可否
   - 実験 3 の手順書ドラフト
2. 実装は 1 変数ずつ。データモデル → 解決規則 → Transport → テストの順で、各段階で `cargo test` を通してから次へ
3. シェル操作は `scripts/hpane.sh` 経由（既定の運用）
4. 仕様の解釈に迷ったら `spec.md` の該当節を引用して質問する。仕様に無い判断をした場合は `results/phase16.md` の「仕様からの逸脱」に必ず記録する

## 完了条件

- [ ] `seqcore` が `no_std` でビルドでき、`cargo test` が全て通る
- [ ] 上記「単体テスト（最低限）」の各項目にテストが存在する
- [ ] `docs/results/phase16.md` に Q1–Q4（`spec.md` §7）の回答が書かれている
- [ ] Host API ギャップ表と、Phase 17 に渡す要求メモが `results/phase16.md` にある
- [ ] `spec.md` の `[Phase で確定]` のうち本 Phase 担当分（上限定数、`PC_LEAD_TICKS` 暫定値）が更新されている
- [ ] `docs/dev-log.md` に実装記録がある

## `docs/results/phase16.md` の雛形

```markdown
# Phase 16 Results

## 実施内容
## seqcore の構成（crate 位置、公開 API 一覧）
## テスト結果（cargo test 出力の要約）
## SL MK3 実験
- PC 反映タイミング:
- 受信チャンネル / Bank Select:
- PC_LEAD_TICKS 暫定値と根拠:
## メモリ見積もり
## Host API ギャップ分析（H1–H6）
## Phase 17 への要求
## 仕様からの逸脱・spec.md への反映
## 残課題
```

## 追記（スコープ変更）

- （日付: 内容）
