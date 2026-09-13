# MidiAppBox ロードマップ

**このファイルがフェーズ計画の唯一の情報源である。**

- フェーズ計画に変更が生じたら、**このファイルだけを更新する**。
- 更新のタイミングは**次フェーズの指示書(`docs/prompts/phaseXX.md`)を書くとき**。
  そのとき、完了したフェーズの実績と、新たに判明した課題を反映する。
- `docs/results/` に書かれたフェーズ計画は**当時の判断の記録(スナップショット)**であり、書き換えない。
- 現在地の詳細は `docs/status.md`、各フェーズの詳細は `docs/results/phaseXX.md` を参照する。

各フェーズは次のサイクルで進める。

```
docs/roadmap.md            ← 本ファイル。フェーズの目的・状態・完了条件の要点
docs/prompts/phaseXX.md    ← フェーズ契約。着手前にコミットし、スコープ変更は末尾に日付付きで追記
docs/results/phaseXX.md    ← 調査・計画・実施記録・実測値・トラブル(実装記録もここ。docs/dev-log.md は 2026-08-23 に廃止)
```

- 状態: `planned` / `in progress` / `done` / `deferred`
- 承認ゲート: Host API / ABI の変更、回帰 conf のスキーマ変更は必ず明示承認を経る
- 各フェーズの指示書は前フェーズの results を前提にして書く。results に書かれていない前提は、指示書に書き込んでから着手する

---

## ① フェーズ計画

### Sequencer App(現在のトラック)

仕様: `docs/apps/sequencer/spec.md`

| Phase | 目的 | 状態 | 完了条件の要点 | 依存 |
|---|---|---|---|---|
| **16** | シーケンサーコア(データモデル + 解決規則 + Transport 状態機械)を host 非依存の `no_std` crate として実装し、単体テストする。~~SL MK3 の PC 挙動を実機で確認する~~(公開仕様どおりと確認済みのため削除)。Host API のギャップを分析する | **done**(2026-09-13) | **`wasm-apps/seqcore/`**(依存 0、no_std、**34 tests**)。Q1: ch16 PC 0..=63 は即時・+64 でパターン末尾へキュー → Session 境界はキューモード、**`PC_LEAD_TICKS = 24`**(暫定)。Q3: 上限定数は据え置き(**Bank 9,842 B**)。Q4: **H1–H6 は既存 API で足りる。新規は H8(テンポ / 拍子マップのリセットと枯渇回避)と H9(境界同期の停止)**。指示書: `docs/prompts/phase16.md` / 記録: `docs/results/phase16.md` | Phase 15 |
| **17** | Host API の追加: **テンポ / 拍子マップのリセットと長時間再生での枯渇回避(H8、必須)、小節境界での停止(H9、推奨)**。Linux/SDL と実機の両方に実装する。拍・小節イベントの通知と境界同期の locate は不要と判定済み(Phase 16) | planned | Host API 仕様の追記、native 実装、ABI 承認の記録。方式案 A / B / C と推奨(B)は `results/phase16.md`「Phase 17 への要求」 | 16 |
| **18** | Session 画面 + 単体再生。メトロノーム、`1` / 繰り返しトグル、点滅表示。Menu と Session 一覧 | planned | `wasm-apps/sequencer` の初回 `.wasm`、SDL と実機での動作動画 | 17 |
| **19** | Song / Chapter 画面と arrangement 再生。Session 境界での PC 送信、SL MK3 との end-to-end | planned | SL MK3 の Session 切り替えデモ動画 | 18 |
| **20** | 永続化(SD カード上の Bank)と最低限の編集(追加 / 削除 / 並べ替え)。Song / Chapter へのトグル横展開の設計 | planned | Bank のファイル形式、編集 UI、Q7 の設計判断 | 19 |
| 21〜 | 拡張: ドラムマシン → コードプレーヤー → フレーズ録音(順序は 20 の完了時に再評価) | deferred | — | 20 |

### マイルストーン

- **Ogaki Mini Maker Faire 2026(12/5–6)**: 出展の最低ラインは Phase 19 の完了(SL MK3 と組み合わせた弾き語りデモ)。出展申込は 9 月中に IAMAS 公式で確認する。

### 完了したフェーズ(要約)

| Phase | 内容 | 完了 | 記録 |
|---|---|---|---|
| 0〜5 | PoC。ESP32-S3 上の WAMR、Host API v0/v1、SD カードからアプリを起動するランチャー(10 サイクル leak-free) | — | `phase00.md` / `phase01-03.md` / `phase04.md` / `phase05.md` |
| 6 | タッチ入力(6A)、MP3 再生とファイル列挙(6B/6C)、デモモード分岐の解消(6D) | — | `phase06.md` |
| 7 | 予約発音(7A)、メトロノーム(7B)、DMA 二重クリック修正(7B-fix)、トーンパレット(7C)、テンポ 1 刻み・音量(7D) | — | `phase07.md` |
| 8a / 8b / 8c | MIDI OUT 疎通 / MIDI Clock 出力 Host API / MIDI IN ハードウェア検証 | — | `phase08a.md` / `phase08b.md` / `phase08c.md` |
| 9a / 9b / 9c | `hostapi_midi_recv` / ループバック診断アプリ / **クロック欠落の原因特定(毎拍の位相リセット)** | 〜2026-08-23 | `phase09a.md` / `phase09b.md` / `phase09c.md` |
| 10 | 新アーキテクチャの調査と設計確定(L0〜L3、Clock Authority、音楽時間軸 API) | 2026-09-05 | `phase10.md` |
| 11 | 音楽時間軸 API 12 関数を実機・Linux の両方に実装(`shared/seq_core.c` に共通化) | 2026-09-06 | `phase11.md` |
| 12 | 基盤整備(16MB パーティション、アプリ 10→6 本、自動回帰スクリプト、PSRAM 可否) | 2026-09-06 | `phase12.md` |
| 13 | metronome を新 API で書き直し。**クロック欠落 0 / 100.00% / BPM 単峰** | 2026-09-06 | `phase13.md` |
| 14 | 旧経路の削除(`click_schedule` / `tone_schedule` / `midi_send` の副作用)。回帰対象は 5 本に | 2026-09-06 | `phase14.md` |
| 15 | PSRAM 本番反映(WASM linear memory と LVGL バッファを PSRAM へ、回帰指標を 4 値に改訂) | 2026-09-12 | `phase15.md` |
| (番外) | check-workflow / check-workflow-routine(herdr 運用の確立)、av-sync-fix、screensaver | 2026-07〜09 | 同名の `docs/results/*.md` |

### 計画の変更履歴

| 日付 | 変更 |
|---|---|
| 2026-09-13 | **Phase 16 完了。** `wasm-apps/seqcore/`(依存 0・no_std・34 tests)。**Phase 17 の中身を指示書の想定から変えた**: 当初は「境界同期のテンポ / 拍子切替、拍・小節イベント」だったが、H1–H3 は既存 API(未来の at_tick 指定、`get_position` のポーリング)で足りると判定した。song tick を単調なタイムラインとして使えば境界同期の locate も要らない。**残る穴はテンポ / 拍子マップ(H8: `transport_start` で消えない・上限 32 件で長時間再生すると枯渇)と境界停止(H9)**なので、Phase 17 はこの 2 点に絞る。方式は A(seq 制御 op)/ B(`tempomap_clear` + 剪定 + `OP_STOP`、推奨)/ C(start でクリア、既存契約を壊すため不可)を比較済み。**指示書から変えた点**: SL MK3 実験の削除(公開仕様で回答)、`Bank.sessions` を `Option` 配列にしない(niche で `.wasm` が太る) |
| 2026-09-13 | **本ファイルを「① フェーズ計画 / ② フェーズ未割当の課題」の 2 部構成に再編した**(Phase 16 のステップ 0)。あわせて次のずれを直した: (a) 仕様のパスを `docs/sequencer/spec.md` から **`docs/apps/sequencer/spec.md`** へ。(b) サイクル図の `docs/dev-log.md` は 2026-08-23 に `docs/results/` へ分割済みなので削除。(c) 完了フェーズの表を `docs/status.md` と照合して書き直した(旧表は 9a に「MIDI Start/Stop/Continue」と書いていたが、実際の担当は Phase 11 の `transport_*`)。(d) 「割り込み候補」と「保留 / 見送り」を ② に統合した。**MIDI Clock 精度の項目(U-1)は再発が無いことをユーザーが確認したのでクローズし、Phase 19 の依存から外した。** あわせて **Phase 16 のスコープから SL MK3 の実機実験を外した**(SL MK3 の PC 仕様は公開情報どおりとユーザーが確認。指示書に追記済み) |
| 2026-09-13 | ロードマップを新設し、Sequencer トラック(Phase 16〜21)を計画した(`952ebaf`) |

---

## ② フェーズ未割当の課題

**番号を振り直さずに課題を置いておく場所。** 着手できる状態になったら ① のフェーズに移すか、新しいフェーズを起こす。
クローズした課題は行を消さず、取り消し線と経緯を残す。

| # | 課題 | 出所 | 温度感 |
|---|---|---|---|
| ~~U-1~~ | **✅ クローズ(2026-09-13、ユーザー確認。再発なし)。** ~~MIDI Clock 精度: 120bpm の送信が SL MK3 で 115–119bpm と検出される問題~~。原因は Phase 9c で特定した毎拍の位相リセット(9c の BPM 分布の 115 / 120 二峰性と一致)。Phase 11 でグリッド生成に置き換え、Phase 14 で旧経路を削除した。Phase 13 で SL MK3 の検知テンポが表示値と一致することをユーザーが目視で確認し、Phase 15 の T-1 でも欠落 0 / 100.00% | `phase09b.md` / `phase09c.md` / `phase13.md` / `phase15.md`、spec Q8 | — |
| U-2 | **PSRAM のリーク監視が「1 回の起動→停止の差分」までしかない。** 同じアプリを N 回繰り返したときの非減少判定(4c)が未実装。linear memory が PSRAM から取られる以上、ここが実質的な監視点になる | `phase15.md` 申し送り | **高め。** Sequencer の `.wasm`(Bank を持つ)を回帰に加える Phase 18 までに入れたい |
| ~~U-3~~ | **➡ Phase 17 へ移した(2026-09-13)。** ~~テンポ / 拍子マップの上限が 32 件で、エントリを消す語彙が無い~~。演奏中のテンポ変更を小節頭に積み続けると枯渇する(metronome は locate(0) で回避)。Phase 16 の分析で、**`transport_start` でもマップが消えない**ことも分かり、Sequencer の要求 H8 になった | `hostapi.md` §6 要件 1 の注記、`phase16.md`「Host API ギャップ分析」 | — |
| U-4 | **`hostapi_midi_recv` のタイムスタンプに線速補正を適用していない**(意味の変更を伴う) | `hostapi.md` §7 / `architecture.md` §11-4 | フレーズ録音系(21〜)の着手前 |
| U-5 | **内蔵音源ポートの追加(移行ステップ 6)** | `architecture.md` §10、`phase14.md` | ドラムマシン拡張(21〜)の前提 |
| U-6 | **Strategy B(`.wasm` バッファを PSRAM に置く)。** 現状はすべての `.wasm` が 16KB 未満なので internal のまま | `phase15.md`、`architecture.md` §9 | いずれかの `.wasm` が 16KB を超えたとき(Phase 18 で計測。**非ゼロ初期値の static データは `.wasm` を太らせる**点に注意) |
| U-7 | **IDF 6.0 への移行。** `espressif/wasm-micro-runtime` の IDF 6 対応待ち(W^X / `MALLOC_CAP_EXEC`) | `docs/notes/idf6-migration-notes.md` | コンポーネント側の対応が公開されたら |
| U-8 | **SDMMC ネイティブモードと PSRAM の共存。** 同じピンを SDMMC → SPI3 と再初期化する 2 段遷移が PSRAM 有効時に不安定。main は SDSPI 固定で運用中 | `phase15.md` ステップ 4 | SD から高速転送が必要になったときだけ |
| U-9 | **エクスプレッションペダル**(ADS1115 経由の `hostapi_analog_read`) | 旧ロードマップ「割り込み候補」 | Sequencer 完了後 |
| U-10 | **ブラウザを第 3 ホストにする**(TypeScript で Host API を実装)。移植点は `shared/seq_core.c` のフック 7 個 | 旧ロードマップ「割り込み候補」、`phase11.md` | Phase 20 以降。Sequencer が最初の移植対象になる |
| U-11 | **Linux ホストの回帰を `timeout N ./build/midibox_host <wasm>` 方式にする。** SIGTERM が `SDL_QUIT` に変換され、xdotool なしで `app stopped` まで完走する | `docs/lessons.md`(14) | workflow §1 の変更にあたるため**ユーザー承認が要る**。急がない |
| U-12 | **Linux ホストの画面キャプチャの自動化。** この環境(Wayland + XWayland / GNOME)では x11grab が黒画面になる | `check-workflow.md` | 保留 |
| U-13 | **120bpm 以外での系統誤差の確認、Song Position Pointer の送出** | `phase09c.md`(持ち越し)、`hostapi.md` §3 | Song の途中から再生する機能を作るとき(SPP) |
| U-14 | **MIDI IN の受信ダンプ機能は `feature/midi-in-rx-dump` ブランチにしか無い**(main 未マージ) | `phase08c.md` | 必要になったとき |
