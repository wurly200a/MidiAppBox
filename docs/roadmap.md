# MidiAppBox Roadmap

このファイルは Phase 単位の計画と現在地を示す。
各 Phase は次のサイクルで進める。

```
docs/roadmap.md            ← 本ファイル。Phase の目的・成果物・状態
docs/prompts/phaseXX.md    ← Phase 契約。着手前にコミットし、スコープ変更は日付付き追記
docs/results/phaseXX.md    ← Phase 完了時の結果報告（何をやり、何が分かり、何を残したか）
docs/dev-log.md            ← 実装記録の詳細（従来どおり）
```

- 状態: `planned` / `in progress` / `done` / `deferred`
- 承認ゲート: Host API / ABI 変更、conf スキーマ変更は必ず明示承認を経る
- 各 Phase の prompt は前 Phase の results を前提にして書く。results に書かれていない前提は prompt に埋め込んでから着手する

---

## 完了した Phase（要約）

| Phase | 内容 | 状態 |
|---|---|---|
| 〜5 | PoC、Launcher（SD カードからの app 起動、10 サイクル leak-free） | done |
| 9a | `hostapi_midi_recv`、MIDI Start/Stop/Continue | done |
| 12 | PSRAM 初期検討、SDSPI フォールバック | done |
| 15 | WASM 線形メモリの PSRAM 配置、LVGL バッファ移行、回帰スクリプト整備 | done (2026-09) |

---

## Sequencer App（現在のトラック）

仕様: `docs/sequencer/spec.md`

| Phase | 目的 | 主な成果物 | 依存 | 状態 |
|---|---|---|---|---|
| **16** | シーケンサーコア（データモデル + 解決規則 + Transport 状態機械）を host 非依存の `no_std` crate として実装・単体テスト。SL MK3 の PC 挙動を実機で確認。Host API ギャップ分析 | `seqcore` crate、`cargo test` on Linux、`results/phase16.md`（Q1–Q4 の回答、H1–H3 の要否） | Phase 15 | planned |
| **17** | Host API 追加（境界同期のテンポ / 拍子切替、拍・小節イベント）。Linux/SDL と実機の両方で実装 | Host API 仕様追記、native 実装、ABI 承認記録 | 16 の H1–H3 判定 | planned |
| **18** | Session 画面 + 単体再生。メトロノーム、`1` / 繰り返しトグル、点滅表示。Menu と Session 一覧 | `apps/sequencer` の初回 `.wasm`、SDL と実機での動作動画 | 17 | planned |
| **19** | Song / Chapter 画面と arrangement 再生。Session 境界での PC 送信、SL MK3 との end-to-end | SL MK3 の Session 切り替えデモ動画 | 18、MIDI Clock 問題の解決（下記） | planned |
| **20** | 永続化（SD カード上の Bank）と最低限の編集（追加 / 削除 / 並べ替え）、Song / Chapter へのトグル横展開設計 | Bank ファイル形式、編集 UI、Q7 の設計判断 | 19 | planned |
| 21〜 | 拡張: ドラムマシン → コードプレーヤー → フレーズ録音（順序は 20 完了時に再評価） | — | 20 | deferred |

### 割り込み候補（別トラック、Sequencer と並行または先行）

| 項目 | 内容 | 位置づけ |
|---|---|---|
| MIDI Clock 精度 | 120bpm 送信が SL MK3 側で 115–119bpm と検出される問題。loopback 受信 app で 3 仮説を切り分け | **Phase 19 の前提**。Phase 16 と並行して着手可（`hostapi_midi_recv` は既存） |
| エクスプレッションペダル | ADS1115 経由の `hostapi_analog_read` | Sequencer 完了後 |
| ブラウザ第 3 ホスト | TypeScript での Host API 実装 | Phase 20 以降。Sequencer が最初の実用 app として移植対象になる |

### マイルストーン

- Ogaki Mini Maker Faire 2026（12/5–6）: Phase 19 完了（SL MK3 と組み合わせた弾き語りデモ）が出展の最低ライン。出展申込は 9 月中に IAMAS 公式を確認

---

## 保留 / 見送り

| 項目 | 理由 | 再開条件 |
|---|---|---|
| IDF 6.0 移行 | `espressif/wasm-micro-runtime` の IDF 6 対応待ち（W^X / `MALLOC_CAP_EXEC`） | コンポーネント側の対応公開。ノートは `docs/notes/idf6-migration-notes.md` |
| Strategy B（`.wasm` バッファの SPIRAM 配置） | 現状 `.wasm` が ~16KB 未満 | Sequencer app が 16KB を超えたとき（Phase 18 で計測） |
