# Phase 16 実施記録 — Sequencer コア(host 非依存)と SL MK3 PC 挙動確認

- 指示書: `docs/prompts/phase16.md`
- 仕様: `docs/apps/sequencer/spec.md`(v0.1 draft)
- 位置づけ: `docs/roadmap.md`

---

## ステップ 0: 着手前の調査報告(2026-09-13、**承認済み**)

> **承認結果(2026-09-13)**: 0-6 の 1〜4 は承認。**SL MK3 の実機実験(0-3)は不要**(下記の公開仕様どおりであることをユーザーが確認済み。指示書に追記した)。**D6 はクローズ**(再発なし)。
> 以降の 0-3 は、実施しなかった計画として残す。

指示書「進め方 1」に従い、実装前に以下を報告して承認を待つ。

### 0-0. 環境確認(workflow §3.0)

- `hpane.sh ensure unix-build` を 2 回実行し同一 pane ID(`wF:pA`)を確認。`run` の echo テストは exit 0。
- Rust: `cargo 1.95.0` / `rustc 1.95.0`。target は `wasm32-unknown-unknown` と `x86_64-unknown-linux-gnu` が導入済み。
- ALSA: `/dev/snd/seq` にアクセスでき、`aconnect -l` に `UM-ONE` が見える(client 20)。
  **Linux ホストから UM-ONE 経由で MIDI を実送信できる**(lessons の「リモートデスクトップだと不可」には該当しない)。

### 0-1. 既存アプリの crate 構成と seqcore の置き場所

**実物の確認結果**(指示書は `apps/` と書いているが、実際のディレクトリは **`wasm-apps/`**):

- `wasm-apps/<app>/` ごとに**独立した crate**。workspace は無い。
  - `Cargo.toml`: `crate-type = ["cdylib"]`、release は `opt-level="z"` / `lto` / `panic="abort"` / `strip`
  - `.cargo/config.toml`: `build.target = "wasm32-unknown-unknown"`、`-zstack-size=8192`
  - **依存 crate は 5 本とも 0 個**(`Cargo.lock` は自分自身のみ)
  - `#![no_std]` + 自前 `panic_handler`、Host API は `extern "C"` で直接宣言
- ビルドはホスト側で `cargo build --release` → `.wasm` をコミット(`wasm-apps/README.md`)。
- `.gitignore` は `wasm-apps/*/target/`。

**提案: `wasm-apps/seqcore/`(アプリと同じ階層の兄弟ディレクトリ)**

| 候補 | 評価 |
|---|---|
| **`wasm-apps/seqcore/`(推奨)** | Phase 18 の `wasm-apps/sequencer/` から `seqcore = { path = "../seqcore" }` で参照する。`target/` は既存の gitignore パターンにそのまま一致する |
| `wasm-apps/sequencer/seqcore/` | **Phase 18 で `wasm-apps/sequencer/.cargo/config.toml`(target=wasm32)を置いた瞬間、cargo の設定探索が親ディレクトリを辿るため、seqcore 内の `cargo test` が wasm32 向けにビルドされて走らなくなる**。子側で上書きすれば回避できるが、罠を 1 つ抱えることになる |
| リポジトリ直下(`crates/seqcore/` 等) | 置けるが、Rust の成果物が `wasm-apps/` の外に散る。gitignore の追加も要る |

crate の形:

- `#![cfg_attr(not(any(test, feature = "std")), no_std)]`。`std` feature はテスト用の出口として用意する(指示書の要求)
- `crate-type` は既定の `rlib`(アプリではないので `cdylib` にしない)
- 検証コマンド(いずれも `hpane.sh run unix-build` 経由):
  - `cargo test --features std`(ホスト)
  - `cargo build --target wasm32-unknown-unknown --release`(no_std で通ることの確認)

### 0-2. 依存追加(`heapless` 等)の可否

**提案: 依存は追加しない。** 固定長コンテナ(`FixedVec<T, const N: usize>`: `[T; N]` と `len` を持つだけ。数十行)を seqcore 内に自前で持つ。

理由:

1. **既存 5 アプリは依存 0 個。** CLAUDE.md の「依存追加は最小限」に沿う。
2. **`heapless` はローカルの cargo キャッシュに無い**(`~/.cargo/registry/cache` にあるのは `stable_deref_trait` のみ)。
   入れるならネットワーク取得が必要で、`hash32` 等の推移的依存も付いてくる。
3. spec §3 で使っているのは `heapless::Vec` の「固定容量 + 長さ」だけで、`String` / `IndexMap` などは使っていない。
4. レイアウトが自前で読めるので、メモリ見積もり(Q3)を `size_of` で正確に出しやすい。

`heapless` を使いたい場合は、承認をもらってから `heapless = { version = "0.8", default-features = false }` を取得する。

### 0-3. SL MK3 実験(Q1 / Q2)の手順書ドラフト

#### 事前情報(実験前の仮説)

Novation の公開情報(ユーザーガイド「SL MkIII Session management」。本文は HTTP 403 で直接読めず、検索結果の抜粋のみ確認)によると:

- **Session は ch16 の Program Change で読み込める。既定では即時に切り替わる。**
- **プログラム番号に 64 を足すと Sequencer がキューし、再生中のパターンの終わりで切り替わる。**
- ch16 は Program Change / Song Select などのグローバルチャンネル。

これが正しければ、Q1 は「即時か末尾か」の二択ではなく**両モードが送信側で選べる**ことになり、`PC_LEAD_TICKS` の意味が変わる:

| モード | 送る値 | `PC_LEAD_TICKS` に求められること |
|---|---|---|
| 即時(0..63) | 境界ちょうど、または直前 | 小さく正確に(拍頭の発音に間に合わせる)。**送信タイミングの精度に依存する** |
| キュー(64..127) | 境界より前ならいつでも | 「パターン末尾」の定義(どのトラック長か)さえ合えば、精度を要求しない |

**実験はこの仮説の検証として設計する。**

#### 構成(推奨: Linux ホスト)

```
Linux ホスト(midibox_host + pc_probe.wasm)
  → ALSA → UM-ONE OUT → SL MK3 MIDI IN(DIN)
```

- **推奨理由**: ファームウェアの変更・フラッシュ・SD 操作が不要で、回帰対象 5 本に一切触れない。Linux ホストの送出精度は Phase 11 の実測で σ33µs 程度なので、SL MK3 の挙動を見るには十分。
- 代替(実機): `wasmrt::kAppsDir`(`/sdcard/apps`)の `.wasm` はランチャーが列挙するので、SD に手動コピーすれば**ファーム変更なし**で動かせる。ただし SD を抜き差しする必要がある(lessons 13 の手順)。
- (任意)送出時刻の記録: `aseqdump -p MidiAppBox` で自プロセスの出力ポートを購読できれば、PC の実送出時刻を残せる。購読できない場合は省略する。

#### 使い捨てアプリ `pc_probe`(Host API 変更なし)

- 置き場所: `wasm-apps/pc_probe/`。**ファームへの埋め込み・ランチャーの seed・回帰スクリプトには追加しない**。
- **Phase 完了時に削除する**(Phase 12 / 14 と同じ扱い。どういうケースを流したかは本ファイルに残す)。
- 使う API: `transport_start/stop`、`tempomap_set_tempo/meter`(120bpm・4/4)、`seq_write`(port=DIN_OUT で PC / CC、port=CLICK で可聴クリック)、`draw_text`(現在のケース番号・次の送出位置を表示)。
- **操作なしで自動進行**させる。1 ケース 4 小節で、画面にケース番号を大きく出す。ユーザーは SL MK3 側の観察に集中できる(Linux ホストのクリック自動化は信頼できないため、操作点を減らす)。

120bpm・4/4 では 1 小節 = 3840 tick(PPQN 960)、MIDI Clock 1 発 = 40 tick。以下の「拍 n」は小節内の拍(1 始まり)。

| ケース | 送出 | 位置 | 確認すること |
|---|---|---|---|
| T1 | ch16 PC=1 | 小節の拍 3 | **即時切替か**(拍 3 で変わるか、次の小節頭か) |
| T2 | ch16 PC=0 | 小節頭ちょうど(lead 0) | 新 Session の 1 拍目が鳴るか / 欠けるか |
| T3 | ch16 PC=1 | 小節頭の 1 クロック前(−40 tick) | T2 との比較(即時モードの lead の要否) |
| T4 | ch16 PC=64 | 小節の拍 3 | **キューが効くか**。1 小節パターンの末尾(次の小節頭)で切り替わるか |
| T5 | ch16 PC=66 | 2 小節パターンの Session 再生中、1 小節目の拍 3 | 「パターン末尾」が**パターン長の終わり**なのか、**次の小節頭**なのか |
| T6 | ch16 PC=(再生中と同じ番号) | 拍 3 | 同じ Session を再送したとき、先頭に戻るか無視されるか(同一 Session が連続する arrangement で PC を送るべきかの判断材料) |
| T7 | **ch1** PC=1 | 拍 3 | ch16 以外を無視するか(Q2) |
| T8 | ch16 CC0=0 / CC32=0 → PC=1 | 拍 3 | Bank Select の有無で挙動が変わるか(Q2) |
| T9 | ch16 PC=1 を即時モードで lead を掃引(−120 / −80 / −40 / 0 tick) | 小節頭の手前 | T2/T3 で差が出た場合のみ。`PC_LEAD_TICKS` の暫定値を決める |

#### ユーザーに依頼する準備(SL MK3 側)

1. SL MK3 のクロック受信を **MIDI DIN の外部クロック**に設定する(設定場所はユーザー側で確認)。
2. **既存の Session を上書きしないよう**、空き Session 番号を 3 つ使う。区別しやすい内容にする。
   例: Session A = 1 小節・1 拍目だけ鳴る / Session B = 1 小節・4 拍すべて鳴る / Session C = 2 小節パターン。
   (**Session 番号と PC 番号の対応(0 始まりか 1 始まりか)も T1 で確認する**。表の PC 値は A/B/C に合わせて決め直す。)
3. 必要なら Components 等で SL MK3 のバックアップを取っておく。

#### 実施の流れ

1. `pc_probe` をビルドし、`midibox_host` の単発実行モードで起動(workflow §3.1)。
2. カメラ録画を開始する(`cam-rec.sh captures/phase16`。SL MK3 の画面・パッド LED と PC からのクリック音を同時に撮る)。
3. ユーザーが T1〜T8 を観察する。ケース番号は画面に出る。完了の返答を待つ。
4. 録画を止め、観察結果を本ファイルの「SL MK3 実験」節に記入する。T9 は T2/T3 の結果を見て要否を判断する。

### 0-4. 指示書・仕様・ロードマップとの食い違い(判断を求める)

| # | 箇所 | 記載 | 実際 | 提案 |
|---|---|---|---|---|
| D1 | 指示書「前提」「スコープ」 | 既存 `apps/` | **`wasm-apps/`** | `wasm-apps/` として読み替える(本文は書き換えない) |
| D2 | 指示書「完了条件」 | `docs/dev-log.md` に実装記録 | **`docs/dev-log.md` は存在しない**(2026-08-23 に `docs/results/` へ分割済み。CLAUDE.md もそう規定) | 実装記録は**本ファイルに一本化**する |
| D3 | 指示書「前提」 | MIDI Start/Stop/Continue の送信は Phase 9a で存在 | Phase 9a は `hostapi_midi_recv`。Start/Stop/Continue は **Phase 11 の `transport_*`** が送る。`hostapi_midi_send` で 0xFA/0xFC を送るのは**二重送出になるため禁止**(`hostapi_defs.h`) | H5 は `transport_*` で回答する |
| D4 | 指示書「前提」/ spec §4.3 | `app_tick` のジッタ上限 5.1ms | docs / results に **5.1ms の出典が見つからない**(`hostapi_defs.h` は「~5ms」)。app_tick の周期は 100ms で、Phase 15 T-2 の実行時間は最大 27.5ms | 本 Phase では**前提として使わない**。PC は `seq_write` で tick に予約するので、送出精度は app_tick のジッタに依存しない |
| D5 | spec §3 | `Chapter.sessions: heapless::Vec<SessionId, MAX_BARS_PER_SESSION>` | 容量が「Session あたりの小節数」の定数になっている | 誤記とみなし、`MAX_SESSIONS_PER_CHAPTER = 16` を新設する(「仕様からの逸脱」に記録) |
| D6 | roadmap「割り込み候補」/ spec Q8 | MIDI Clock の 115–119bpm 検出問題が未解決で、**Phase 19 の前提** | Phase 9c で原因を特定(毎拍の位相リセット)。Phase 11 で新経路に置き換え、Phase 14 で旧経路を削除した。**Phase 13(2026-09-06)で SL MK3 の検知テンポが表示値と一致し、一度も外れないことをユーザー目視で確認済み**。Phase 15 の T-1(PSRAM 有効構成)でも欠落 0 / 100.00% | **解決済みとしてクローズしてよいか確認したい**(最近 SL MK3 で再発を見た、などの事情があれば別) |
| D7 | roadmap | 仕様 `docs/sequencer/spec.md` | **`docs/apps/sequencer/spec.md`** | roadmap を修正済み |

### 0-5. 先行所見(実装後に確定させる暫定値)

#### メモリ(Q3)の手計算による概算

`Name = [u8; 16]` と仮定。wasm32 では `usize` = 4B。

| 型 | 概算 | 内訳 |
|---|---|---|
| `Session` | ~70 B | name 16 + bar_meter 16×3(`Option<TimeSig>` は 3B)+ 他 6 |
| `Chapter` | 36 B | name 16 + Vec<u8,16>(16 + len 4) |
| `Song` | ~736 B | chapters 16×36+4、arrangement 36、tempo_triggers 16×6+4、他 18 |
| **`Bank`** | **~10.4 KB** | sessions 64×70 + songs 8×736 |

- **linear memory の上限は問題にならない見込み。** 現状の linear memory は 16.5KB で、PSRAM 上に 8.25MB まで取れる(Phase 15 T-3)。
- **実際に効く制約は 2 つ。実装で扱う:**
  1. **wasm スタックは 8KB(`-zstack-size=8192`)。** `Bank` を値で返したりスタック上に組み立てたりすると溢れる。`static` に置き、参照で扱う。
  2. **非ゼロ初期値の `static Bank` は `.data` として `.wasm` ファイルに載る(+10KB)。** `.wasm` が 16KB を超えると、internal に置いている `.wasm` バッファの Strategy B 条件に触れる。デモ用の曲データは `app_init` で組み立てるか、ゼロ初期化(`.bss`)にする。
- 確定値は `size_of` を出すテストで取る(ステップ 2 以降)。

#### Host API ギャップ(Q4)の暫定表

`shared/hostapi_defs.h` / `docs/hostapi.md` / `shared/seq_core.c` を読んだ結果。

| # | 要求 | 既存 API | 判定(暫定) | 注記 |
|---|---|---|---|---|
| H1 | テンポ(次の小節境界から) | `tempomap_set_tempo(at_song_tick, upq)` | **足りる**(条件付き) | PLAYING 中も未来の at_tick を受け付ける。ただし**エントリ上限 32・削除する語彙が無い**(G2) |
| H2 | 拍子(次の小節境界から) | `tempomap_set_meter(at_song_tick, n, d)` | **足りる**(条件付き) | 同じく上限 32・削除不可。PLAYING 中に過去 tick へ書くと小節番号が遡って変わる(ガード無し) |
| H3 | 拍 / 小節イベント | `transport_get_position`(bar / beat / host_us) | **ポーリングで足りる見込み** | 通知 API は無い。app_tick 100ms 周期なので点滅表示は最大約 100ms 遅れる。発音・PC は tick 予約なので影響しない |
| H4 | PC 送信 | `seq_write(port=DIN_OUT, status=0xC0\|ch, data1=pc)` | **足りる** | `midi_msg_len` が 0xC0 を 2 バイトとして扱う。`hostapi_midi_send` より tick 精度で有利 |
| H5 | Start / Stop | `transport_start` / `transport_stop` | **足りる** | D3 参照 |
| H6 | click ON/OFF・アクセント | `seq_write(port=CLICK, OP_TONE, param=slot)` + `tone_define` | **足りる** | OFF は書かないだけ。アクセントは別スロット(metronome と同じ) |

**既存 API では表現できない境界同期(Phase 17 の本題になる見込み)**:

- **G1: 境界同期の locate が無い。** `transport_locate` は即時のみ。`QueuedAction::Jump` や「`1` トグル ON で選択小節へ移る」は、100ms 周期の app_tick からは小節頭ちょうどに打てない。`set_loop` の終端を使う抜け道は**前方へのジャンプを表現できない**(start >= end は -1)。
- **G2: テンポ / 拍子マップにエントリを消す語彙が無い。** 同じ Session を arrangement で何度も使い、小節単位で拍子を上書きすると、上書きと戻しで 2 エントリずつ消費し 32 件で枯渇する(hostapi.md §6 要件 1 の注記で予告されていた制約)。
- **G3: 境界同期の停止が無い。** 「全小節を再生して停止」を小節頭ちょうどで止める手段が無い。app_tick で止めると最大約 100ms 遅れ、その間クロックが流れる。

Phase 17 への方式案(2 案以上。**本 Phase では実装しない**):

- **方式 A: seq の制御オペコード**(`HOSTAPI_SEQ_OP_LOCATE` / `OP_STOP` / `OP_TEMPO` / `OP_METER`)。playback tick 指定で L0 が実行する。`HOSTAPI_SEQ_OP_*` は「追加は非破壊」と予約済みで、**関数は増えない**(hostapi.md §0 の検証観点に沿う)。既存の先読み供給ループにそのまま乗る。論点は、`get_position` の bar / beat 算出(song tick 上のマップ)との整合と、キュー 256 件の消費。
- **方式 B: 専用関数**(`transport_locate_at(at, to)` / `transport_stop_at(at)` / `tempomap_clear_after(tick)`)。意味は明快だが語彙が増える。
- **方式 C: song tick を「演奏タイムライン」として単調に使い、locate / loop を使わない。** ホストは通過済みのマップエントリを自動で剪定するだけ(ABI 不変・挙動変更)。停止だけは A か B が要る。

どれを推奨するかは、seqcore の境界イベント出力の形が決まってから本ファイルの「Phase 17 への要求」に書く。

### 0-6. 承認をお願いしたい事項

1. **seqcore の置き場所**: `wasm-apps/seqcore/`(0-1)
2. **依存を追加しない**(自前の `FixedVec`)(0-2)
3. **SL MK3 実験**: Linux ホスト + 自動進行の使い捨て `pc_probe`(Phase 完了時に削除)と、上の手順書(0-3)
4. **D2**(実装記録は本ファイルに一本化)・**D5**(`MAX_SESSIONS_PER_CHAPTER` の新設)
5. **D6**: MIDI Clock 115–119bpm 問題をクローズしてよいか

---

## 実施内容

2026-09-13 に実施。完了条件の充足状況:

| 完了条件 | 状況 |
|---|---|
| `seqcore` が `no_std` でビルドでき、`cargo test` が全て通る | **達成**。wasm32(no_std)ビルド成功、`cargo test --features std` で 34 passed / 警告 0 |
| 「単体テスト(最低限)」の各項目にテストが存在する | **達成**(下記「テスト結果」の対応表) |
| Q1–Q4 の回答 | **達成**(Q1 / Q2 は SL MK3 の公開仕様から回答。実機実験はスコープから外した) |
| Host API ギャップ表と Phase 17 への要求メモ | **達成** |
| spec.md の本 Phase 担当分(上限定数、`PC_LEAD_TICKS` 暫定値)の更新 | **達成** |
| `docs/dev-log.md` に実装記録 | **本ファイルに一本化**(ステップ 0 の D2 で承認) |

進めた順番(指示書「進め方 2」: 各段階で `cargo test` を通してから次へ):

1. **データモデル**(`fixed.rs` / `model.rs`)。初回のテストで `empty_bank_is_all_zero_bits` が落ちた
   (`Bank::new()` の 9,842 B のうち 64 B が非ゼロ)。原因は `Option<Session>` の niche 最適化で、
   `Bank.sessions` の表現を変えて解消した(「仕様からの逸脱」3)。**10 passed**。wasm32 ビルド成功。
2. **解決規則**(`resolve.rs`)。**17 passed**。
3. **Transport**(`transport.rs`)。**34 passed**、警告 0。wasm32 ビルド成功。

シェル操作はすべて `hpane.sh run unix-build` 経由で行った。ログは `captures/phase16/test-stage{1,2,3}.log` / `wasm-stage{1,2,3}.log`(.gitignore 対象)。

**ファームウェア・Linux ホスト・`shared/`・既存 5 アプリ・Host API には触れていない**ので、実機 / Linux の回帰は行っていない
(本 Phase の変更は `wasm-apps/seqcore/` の新設とドキュメントのみ)。

## seqcore の構成(crate 位置、公開 API 一覧)

位置: **`wasm-apps/seqcore/`**。依存 crate 0 個、`#![cfg_attr(not(any(test, feature = "std")), no_std)]`、rlib。

| ファイル | 内容 |
|---|---|
| `src/fixed.rs` | `FixedVec<T: Copy, const N>`: `new(fill)`(const)/ `capacity` / `push` / `extend_from_slice` / `pop` / `clear` / `Deref<[T]>`。長さは `u8` |
| `src/model.rs` | 上限定数、`Name`(`EMPTY` / `new` / `as_str`)、`TimeSig`(`FOUR_FOUR` / `new`)、`Session`(`new` / `with_name` / `with_bar_meter`)、`Chapter`(`EMPTY` / `new`)、`SongPos`(`START` / `new`、導出順 = 曲の進行順)、`TempoTrigger`、`Song`(`EMPTY` / `new` / `session_at` / `first_pos` / `next_pos`)、`Bank`(`new`(const)/ `session` / `set_session`) |
| `src/resolve.rs` | `effective_tempo(song, pos) -> u16`、`effective_meter(session, bar) -> TimeSig` |
| `src/transport.rs` | 定数 `DEFAULT_BPM` / `PC_CHANNEL` / `PC_CUE_OFFSET` / `PC_LEAD_TICKS`、`ProgramChange`(`bytes`)、`Scope`、`Position`、`QueuedAction`、`State`、`BarEvents`、`Error`、`Transport` |

`Transport` のメソッド:

| メソッド | 役割 |
|---|---|
| `new`(const)/ `state` / `is_playing` / `position` / `bpm` / `meter` | 生成と参照 |
| `play_song(bank, song)` / `play_session(bank, session, single_bar, repeat)` | 再生開始。開始時の `BarEvents`(start / PC / テンポ / 拍子)を返す |
| `set_bpm(bpm)` | Transport の現在テンポ。停止中は即時、Session scope 再生中は次の小節境界から、Song scope では `WrongScope` |
| `set_session_toggles(bank, single_bar, repeat)` | トグル。次の小節境界から効く |
| `queue(bank, action)` | `Stop` / `Jump(SongPos)`(Song scope)/ `JumpBar(u8)`(Session scope)を次の境界に予約 |
| `advance_bar(bank)` / `advance_beat(bank)` | 境界で前進し、`BarEvents` を返す。進む先が無ければ停止(`stop: true`) |
| `stop()` | 即時停止 |

**境界イベントは値で返す**(MIDI 送信はしない):

```rust
pub struct BarEvents {
    pub start: bool,                 // MIDI Start
    pub stop: bool,                  // MIDI Stop(この境界で終了)
    pub pc: Option<ProgramChange>,   // { program, cue } → bytes() で [0xCF, n]
    pub tempo: Option<u16>,          // 変わったときだけ(開始時は必ず)
    pub meter: Option<TimeSig>,      // 同上
}
```

`Transport` は `Copy` なので、境界より前に次の小節を知りたいときは複製して `advance_bar` すればよい
(PC を `PC_LEAD_TICKS` 手前に予約するための先読み)。

## テスト結果(cargo test 出力の要約)

```
$ cargo test --features std
running 34 tests
...
test result: ok. 34 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out
$ cargo build --release --target wasm32-unknown-unknown
    Finished `release` profile [optimized] target(s)
```

指示書「単体テスト(最低限)」との対応:

| 要求 | テスト |
|---|---|
| テンポ解決: トリガー無し | `resolve::tempo_without_triggers_is_the_song_default` |
| テンポ解決: 先頭 | `resolve::tempo_trigger_at_the_song_start_replaces_the_default` |
| テンポ解決: 途中 | `resolve::tempo_trigger_midway_applies_from_its_own_bar` |
| テンポ解決: 途中 Session から開始 | `resolve::tempo_from_a_middle_session_sees_every_earlier_trigger`(+ 並び順非依存 `tempo_does_not_depend_on_trigger_order`) |
| 拍子解決: Session 既定 | `resolve::meter_defaults_to_the_session_meter` |
| 拍子解決: 小節上書き | `resolve::meter_bar_override_applies_only_to_that_bar` |
| Song 全体を 1 回再生して停止 | `transport::song_plays_the_whole_arrangement_once_then_stops`(+ 各小節の PC / 拍子 `song_events_at_each_bar_of_the_sample`) |
| Session 4 通りのトグル | `session_all_bars_once_then_stops` / `session_all_bars_repeat_loops` / `session_single_bar_once_then_stops` / `session_single_bar_repeat_loops_that_bar` |
| 再生中のトグル変更が次の小節境界で効く | `transport::toggle_change_during_playback_takes_effect_at_the_next_bar` |
| `QueuedAction::Jump` | `queued_jump_executes_at_the_next_bar_boundary`(テンポ再評価・PC 含む)/ `queued_jump_within_the_same_session_sends_no_pc` / `queued_stop_and_jump_bar` |
| 同一 Session を 2 つの Chapter から参照 → Session 境界ごとに PC | `transport::pc_is_sent_at_every_session_boundary_of_a_shared_session` |

その他: Session scope のテンポ(`session_scope_uses_the_transport_tempo`)、拍の繰り上がり(`beats_roll_over_into_the_next_bar_by_the_meter`、3/4 と 7/8)、
入力検証(`play_is_validated` / `toggles_are_validated`)、PC バイト列(`program_change_bytes_for_sl_mk3`)、
モデル(参照切れのスキップ、名前の切り詰め、`FixedVec`)、メモリ(`layout_sizes_for_the_memory_estimate` / `empty_bank_is_all_zero_bits`)。

## SL MK3(Q1 / Q2)

実機実験は行わず、Novation の公開仕様(ユーザー確認済み)から回答した(指示書の追記 2026-09-13)。

- **PC 反映タイミング(Q1)**: ch16 の PC 番号 0..=63 は **即時** に Session が切り替わる。**+64 で再生中パターンの末尾へキュー**される。
  Sequencer は **Session 境界ではキューモード、再生開始時は即時モード** を使う(`ProgramChange.cue`)。
- **受信チャンネル / Bank Select(Q2)**: **ch16**(`PC_CHANNEL = 15`)。**Bank Select は不要**(64 Session が PC 0..=63 に収まる)。
  Session 番号と PC 番号の対応は **0 始まり(SL MK3 の Session 1 = PC 0)と仮定**している。Phase 19 の end-to-end で確かめる。
- **`PC_LEAD_TICKS` 暫定値と根拠**: **24(24ppqn = 4 分音符 1 つ。内部 PPQN 960 では 960 tick)**。
  1. キューモードでは切替の瞬間を SL MK3 自身のパターン末尾が決める。送信側に要るのは
     「最後のパターン周回に入ってから、境界より前に届く」ことだけで、境界ちょうどに合わせる精度は要らない。
  2. 手前すぎると 1 つ前のパターン周回の末尾で切り替わってしまう。パターン長が 4 分音符以上なら、4 分音符 1 つ手前は必ず最後の周回に入っている。
  3. 遅延に対する余裕: 240bpm でも 250ms。DIN の 2 バイトは 640µs、L0 の発火偏差は最悪 104µs(P10-3)なので桁違いに大きい。
  4. PC は `seq_write(port=DIN_OUT)` で playback tick に予約するので、`app_tick`(100ms 周期)のジッタに依存しない。
  - 即時モードを境界で使わない理由: 境界ちょうどに届ける必要があり、SL MK3 側の処理遅延で新 Session の 1 拍目が欠けるおそれがある。
- **Phase 18 / 19 の実装メモ**:
  - **再生開始時の PC は `hostapi_midi_send` で `transport_start` の前に送る。** `transport_start` は L0 のキューを空にする(`seq_core.c` の `s_count = 0`)ので、`seq_write` で先に積んでも消える。
  - Session 境界の PC は「次の小節の先頭 playback tick − 960」に `seq_write` する。

## メモリ見積もり(Q3)

`size_of` の実測(`layout_sizes_for_the_memory_estimate`)。**長さを `u8` で持つのでホストと wasm32 で同じ値になる**。

| 型 | ステップ 0 の手計算 | **実測** | 差の理由 |
|---|---|---|---|
| `Session` | ~70 B | **69 B** | — |
| `Chapter` | 36 B | **33 B** | 長さが `usize`(4B)ではなく `u8` |
| `Song` | ~736 B | **678 B** | 同上(FixedVec 3 個) |
| `Bank` | ~10.4 KB | **9,842 B** | 64×69 + 8×678 + 長さ 1 + パディング |
| `Transport` | — | **24 B** | |

**判定: 上限定数は spec §3 の値のまま確定し、下げない。**

- **linear memory には収まる。** 現状のアプリの linear memory は 16.5KB で、`static` の Bank を足すと約 26KB になる。
  linear memory は PSRAM 上にあり、最大連続ブロック(8.25MB、Phase 15 T-3)の 1/300 以下。internal RAM は消費しない。
- **実際に効く制約(Phase 18 のアプリで守ること)**:
  1. **`Bank` は `static` に置き、ゼロ初期化のまま使う。** 初期値に非ゼロがあると 9.8KB 全体が `.wasm` の .data に載り、
     `.wasm` が 16KB を超えて Strategy B(roadmap U-6)に触れる。デモ曲は `app_init` で組み立てる。
     `Bank::new()` が全ビット 0 であることはテストで固定した。
  2. **wasm スタックは 8KB。** `Bank`(9.8KB)は値で扱わない(返り値・ローカル変数にしない)。`Song`(678B)は値で扱えるが、多重にコピーしない。
- 永続化(Phase 20)で Bank をそのまま書くなら約 9.8KB / ファイル。

## Host API ギャップ分析(H1–H6)

`shared/hostapi_defs.h` / `docs/hostapi.md` / `shared/seq_core.c` を読んで判定した。**本 Phase で Host API は変更していない。**

| # | 要求 | 使う既存 API | 判定 | 根拠 / 制約 |
|---|---|---|---|---|
| H1 | テンポ(次の小節境界から) | `tempomap_set_tempo(at_song_tick, upq)` | **既存で足りる**(H8 の制約つき) | PLAYING 中も未来の at_tick を受け付け、区間境界を張り直す(`seg_recompute_end_locked`)。 |
| H2 | 拍子(次の小節境界から) | `tempomap_set_meter(at_song_tick, n, d)` | **既存で足りる**(H8 の制約つき) | 同上。click のアクセントはアプリが seq_write で置くので、拍子マップはクリック位置に影響しない |
| H3 | 拍 / 小節イベント | `transport_get_position`(bar / beat / tick / host_us) | **ポーリングで足りる**。新 API 不要 | 表示(点滅)は最大 1 app_tick 遅れる。発音・PC・テンポは tick 予約なので影響しない。小節の進行は seqcore の `advance_bar` をアプリが tick から駆動する |
| H4 | PC 送信 | `seq_write(port=DIN_OUT, 0xCF, n)` / 開始時は `midi_send` | **既存で足りる** | `midi_msg_len` が 0xC0 系を 2 バイトで送る。開始時の注意は上記「実装メモ」 |
| H5 | Start / Stop | `transport_start` / `transport_stop` | **既存で足りる** | `midi_send` で 0xFA / 0xFC を送らない(二重送出) |
| H6 | click ON/OFF・アクセント | `seq_write(port=CLICK, OP_TONE, param=slot)` + `tone_define` | **既存で足りる** | metronome(Phase 13)と同じ |

### 既存 API で足りない点

**アプリの時間軸の使い方(前提)**: Sequencer は **song tick を「演奏タイムライン」として単調に使い、`transport_locate` / `tempomap_set_loop` を使わない**。
曲構造(繰り返し・ジャンプ・トグル)の解釈はすべて seqcore が持ち、ホストには「次の小節に何を鳴らすか」だけを先読みで書く。
理由: ステップ 0 の G1 で見たとおり、`transport_locate` は即時しか無く、`set_loop` は前方へのジャンプを表現できない。
100ms 周期の app_tick から小節頭ちょうどに打つことはできない。
タイムラインを単調にすれば、ジャンプやトグル変更は「まだ鳴っていない先の小節の内容が変わる」だけになる。
先に書いた分は `seq_flush_after(次の境界の playback tick)` で捨てて書き直し、テンポ / 拍子は同じ at_tick への上書きで直せる。
**したがって G1(境界同期の locate)は新 API を要求しない。**

その代わり、次の 2 点が表に出る:

- **H8(必須)テンポ / 拍子マップ**
  - **(a) 再生を始め直してもマップが残る。** マップを消すのはアプリ破棄時の `seqcore_reset`(`reset_state_locked` で `s_tempo_n = 0`)だけで、
    `transport_start` は消さない。同じアプリで 2 回目の再生をすると、前回の再生で未来の tick に書いたエントリが効いてしまう。
    アプリからエントリを消す語彙は無い。
  - **(b) 長く再生すると枯渇する。** タイムラインが単調なので、テンポ / 拍子の変化 1 回ごとに新しい at_tick のエントリを 1 件消費する(上限 `SEQCORE_TEMPO_MAX` / `SEQCORE_METER_MAX` = 32)。
    例: 2 小節の Session のうち 1 小節だけ 7/8、を繰り返し再生すると 1 周 2 件で、**16 周(120bpm で約 1 分)で溢れる**。
    溢れると `set_meter` が -1 を返し、以後の拍子変更がホストに届かない。
- **H9(推奨)小節境界ちょうどでの停止**
  「全小節を再生して停止」「QueuedAction::Stop」を小節頭で止める手段が無い。app_tick から `transport_stop` を呼ぶと最大約 100ms 遅れ、その間クロックが流れる(120bpm で約 5 発)。
  SL MK3 が次の周回の頭を鳴らしうる。**回避策はある**: `seq_write(port=DIN_OUT, status=0xFC)` を境界の tick に積む。
  `midi_msg_len` は 0xFC を 1 バイトで送り、Stop 後のクロックは受信側で進行に使われない。
  ただし `hostapi_defs.h` が「リアルタイムバイトは transport_* を使う」と定めた規約に反し、ホストの状態(PLAYING)とも食い違うので推奨しない。

## Phase 17 への要求

**要求 1(必須、H8)**: 同じアプリの中で再生を何度始め直しても、また長時間再生しても、テンポ / 拍子の予約が正しく効くこと。

**要求 2(推奨、H9)**: 小節境界の playback tick を指定して停止できること(MIDI Stop とクロック停止が同じ tick で起きる)。

**不要と判定したもの**: 拍 / 小節イベントの通知 API(H3)、境界同期の locate(G1)。

方式案(**Phase 17 の指示書で選び、承認ゲートを通す**):

| | 方式 A: seq の制御オペコード | 方式 B: 最小の関数追加 + 剪定 | 方式 C: `transport_start` でマップを消す |
|---|---|---|---|
| 内容 | `HOSTAPI_SEQ_OP_TEMPO` / `OP_METER` / `OP_STOP` を追加し、L0 が playback tick で「現在値」を差し替える。マップは初期値だけに使う | `hostapi_tempomap_clear()`(STOPPED のみ)を追加。PLAYING 中はホストが**通過済みエントリを自動で剪定**する(ループ未設定時のみ)。停止は `HOSTAPI_SEQ_OP_STOP` | 関数を足さず、`transport_start` がマップを消す挙動変更 + 剪定 + `OP_STOP` |
| 語彙 | 関数 0(op 追加のみ。「追加は非破壊」と予約済み) | 関数 +1、op +1 | 関数 0、op +1 |
| 枯渇 | **構造的に起きない** | 剪定で起きない(先読み範囲に 32 件を超える変化を積まない限り) | 同左 |
| 影響範囲 | 大きい。`get_position` の bar / beat は拍子マップから算出している(`bar_beat_locked`)ので増分計算に変わる。テンポ区間の境界(`seg_end`)を op の発火から作るためディスパッチャに手が入る。キューを小節あたり最大 3 件消費する | 小さい。算出ロジックはそのまま。剪定は「現在区間より前のエントリを 1 件残して詰める」だけ | **不可**。「STOPPED 中に `set_tempo(0, …)` で初期テンポを設定してから `transport_start`」という既存の契約(`hostapi.md` §4 / §6 要件 1)を壊し、metronome 等に影響する |
| 既存アプリ | 無影響(新 op を使わない) | 無影響(ループを使う metronome 系は剪定対象外) | 影響あり |

**推奨: 方式 B。** 変更が L1 の中に閉じ、既存アプリと bar / beat 算出に触れずに要求 1 / 2 を満たす。語彙を増やさないことを優先するなら方式 A を検討する。

Phase 17 で合否に使う検証シナリオ(案):

1. 2 小節の Session のうち 1 小節を 7/8 にして 10 分間繰り返し再生し、`set_meter` が一度も -1 を返さず、拍子が毎周正しいこと
2. 同じアプリで Song をテンポ違いで 2 回続けて再生し、2 回目に 1 回目のテンポ変化が混ざらないこと
3. 境界停止で、MIDI Stop 以降にクロックが 0 発であること(`midi-clock-probe` で確認)

## 仕様からの逸脱・spec.md への反映

| # | 逸脱・解釈 | 理由 | spec.md |
|---|---|---|---|
| 1 | `heapless::Vec` → 自前の `FixedVec`(長さ `u8`) | 依存を増やさない(承認済み)。ホストと wasm32 でレイアウトを一致させる | §3 に反映 |
| 2 | `MAX_SESSIONS_PER_CHAPTER = 16` を新設 | `Chapter.sessions` の容量が `MAX_BARS_PER_SESSION` だったのは誤記(D5、承認済み) | §3 に反映 |
| 3 | `Bank.sessions` は `[Session; 64]`(`bars == 0` が空き)。`Bank::session(id) -> Option<&Session>` で読む | `Option<Session>` の None が非ゼロ表現(実測 64 B)で、static の Bank が .data に載り `.wasm` が約 9.8KB 太る | §3 に反映 |
| 4 | `Name = [u8; 16]`(UTF-8、NUL 詰め、文字境界で切り詰め) | spec は `[Phase で確定]` だった | §3 に反映 |
| 5 | spec の `enum Transport` を `State` とし、`struct Transport { state, bpm, requested_bpm, meter }` で包んだ | Session scope が使う「Transport の現在テンポ」(§3.2)の置き場所と、テンポ / 拍子の「変化」を検出するための現在値が要る | §4 に反映 |
| 6 | `QueuedAction::JumpBar(u8)` を追加 | `Jump(SongPos)` は Session scope(Session 画面の小節ジャンプ、§5.3)を表現できない | §4 に反映 |
| 7 | 再生中に `1` を ON にしたら、次の境界で選択小節へ移り、矢印 OFF なら弾き終えて停止 | spec §4.1 に途中変更時の遷移先の規定が無い | §4.1 に反映 |
| 8 | Session scope 再生中の `set_bpm` は次の小節境界から効く。Song scope では `WrongScope` | H1(「次の小節境界から有効」)に合わせた。Song のテンポは TempoTrigger が決める | 反映なし(実装の API) |
| 9 | 再生中に `play_*` を呼ぶと `AlreadyPlaying` | 再生を切り替えるときは Stop → Start を明示させる(MIDI Stop / Start を必ず出す) | 反映なし |
| 10 | 参照切れ(Bank に無い Session、Song に無い Chapter)と 0 小節の Session は、再生時に飛ばす | spec に規定が無い。止まるより飛ばす方が「編集途中のデータで再生できない」を避けられる | 反映なし |
| 11 | PC の要否は「arrangement 上の Session 枠 `(arr_idx, sess_idx)` が変わったか」で決める。同じ枠の中の Jump では送らない | spec §4.2「Session 境界」の解釈。同じ Session が別の枠で続く場合は送る(指示書のテスト要求どおり) | §4.2 に反映 |
| 12 | `effective_tempo` は `tempo_triggers` の並び順に依存しない | spec は at 昇順を前提にしているが、編集(Phase 20)で崩れても誤動作しないようにした | 反映なし |

あわせて spec.md に反映した確定事項: §4.2 の SL MK3 PC 仕様と `PC_LEAD_TICKS`、§6 の H1–H6 の判定と H8 / H9 の追加、§7 の Q1–Q4 の回答と Q8 のクローズ。

## 残課題

- **Session scope の再生で SL MK3 を追従させるか。** spec §4.2 に従い Session scope では PC を送らないので、Session 画面から再生しても SL MK3 は別の Session のままになりうる。**Phase 18 で判断**(開始時に即時 PC を送るのが自然に見える)。
- **小節途中への Jump(Song scope)では SL MK3 とパターン位置がずれる。** SL MK3 は Session を途中から始められない(キューされた Session は頭から始まる)。ジャンプ先を Session の先頭に限定するかを **Q5(Phase 18)と合わせて判断**。
- **`PC_LEAD_TICKS` の前提の確認**(Phase 19 の end-to-end): SL MK3 のパターンが 4 分音符より短くないこと、トラックごとにパターン長が違うときの「パターン末尾」の定義、PC 番号が 0 始まりであること、同じ Session をキューで再送したときの挙動(境界と一致するので実害は無い見込み)。
- **不正値の検証はしていない**(`bpm = 0`、`den = 0`、`program > 63` など)。`ProgramChange::bytes` は上位ビットを落とすだけ。入力時の検証は編集 UI(Phase 20)で行う。
- **Phase 18 のアプリ設計への申し送り**: song tick を単調なタイムラインとして使う(上記「前提」)。先読み horizon を 1〜2 小節にすると、トグル変更で書き直す範囲が決まる。境界まで 1 app_tick 未満で変更された場合は、次の次の境界から効く扱いにするなど、締め切りを決める必要がある。
- roadmap U-2(PSRAM リークの N 回反復判定)は、Sequencer の `.wasm`(Bank を持つ)を回帰に加える前に入れる。
