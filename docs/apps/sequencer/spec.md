# Sequencer App — 要求・仕様 (v0.1 draft)

- 対象: MidiAppBox 上で動作する最初の本格アプリ「Sequencer」
- 状態: 設計ドラフト。Phase 16 以降で確定する項目は `[Phase で確定]` と明記する。
  **Phase 16 で確定した項目は `[Phase 16 で確定]` とし、根拠は `docs/results/phase16.md` にある**
- 元資料: `MetronomeAppSpec.pptx`（画面ラフ）、2026-09-13 のレビュー議論
- 実装: コア（§3 データモデル・§3.2 解決規則・§4 Transport）は `wasm-apps/seqcore/`（Phase 16）

---

## 1. 目的とスコープ

### 1.1 目的

通常の構成を持つ西洋ポピュラー音楽の構造（曲 → セクション → フレーズ → 小節）をモデルとして持ち、
小節構成・拍子・テンポに従って再生できるシーケンサーを作る。

第一弾（v1）のゴール:

1. メトロノームを鳴らせる
2. 曲構成に沿って Program Change（PC）を送信できる
3. 上記により **Novation SL MK3 の Session 切り替えマスター** として機能する

### 1.2 将来拡張（v1 では非対象だが、データ構造は壊さない）

- ドラムマシン / サンプラー
- MIDI 和音プレーヤー
- MIDI フレーズレコーディング
- 弾き語り用簡易バッキング（ドラム / ベース / コードを単独または組み合わせ）

拡張はいずれも **Session に内容（パターン・クリップ）をぶら下げる** 形で入る想定。
v1 の Session は「長さ・拍子・PC 番号だけを持つ空の容れ物」である。

### 1.3 v1 の非目標

- 曲データの編集 UI（追加・削除・並べ替え）→ 後続 Phase
- SD カードへの永続化 → 後続 Phase
- Song / Chapter 画面での再生トグル（1 / 繰り返し）→ Session 画面のみで先行実装し、後で横展開
- 拍より細かいテンポ変化（rit. / accel.）
- スイング、カウントイン → `[Phase で確定]`（v1 に入れるかは Phase 18 で判断）

---

## 2. 用語

| 用語 | 意味 | 音楽的対応 | SL MK3 対応 |
|---|---|---|---|
| Song | 1曲。Chapter の並び（arrangement）とテンポトリガーを持つ | 曲 | — |
| Chapter | Song 内のセクション定義。Session への参照列を持つ | Intro / A / B / サビ | — |
| Session | **グローバル**な単位。小節数・拍子・PC 番号を持つ | フレーズ（典型 8 小節） | Session（PC で選択） |
| Bar | Session 内の 1 小節。拍子の上書きのみ持てる | 小節 | — |
| TempoTrigger | Song 内の位置に置く「ここからこのテンポ」 | テンポ指示 | MIDI Clock レートに反映 |
| Transport | 装置に 1 つだけ存在する再生機構 | — | Start / Stop / Clock 送信元 |
| Scope | 再生範囲（どの階層の何を再生するか） | — | — |

### 参照の原則

- Session はグローバル。複数の Chapter から同じ Session を参照できる
- Chapter は Song 内で定義され、arrangement から参照される。`A B C A B` の 2 つの A は **同じ定義への参照**
- したがって「A を直せば全部直る」「Session 1 を直せば全曲に効く」

---

## 3. データモデル

`no_std` / 固定長を前提とした Rust 表現。上限値は **Phase 16 のメモリ見積もりで確定** した（`[Phase 16 で確定]`、Bank 全体で 9,842 B）。

```rust
pub type SessionId = u8;   // 0..=MAX_SESSIONS-1
pub type ChapterIdx = u8;  // Song 内インデックス

pub const MAX_SESSIONS: usize = 64;          // SL MK3 の Session 数（PC 0..=63） [Phase 16 で確定]
pub const MAX_BARS_PER_SESSION: usize = 16;
pub const MAX_SESSIONS_PER_CHAPTER: usize = 16; // [Phase 16 で追加] 旧版は MAX_BARS_PER_SESSION を流用していた
pub const MAX_CHAPTERS_PER_SONG: usize = 16;
pub const MAX_ARRANGEMENT_LEN: usize = 32;
pub const MAX_TEMPO_TRIGGERS: usize = 16;
pub const MAX_SONGS: usize = 8;
pub const NAME_LEN: usize = 16;

pub struct Name([u8; NAME_LEN]);               // UTF-8、NUL 詰め、文字境界で切り詰め [Phase 16 で確定]

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct TimeSig { pub num: u8, pub den: u8 }   // 4/4, 3/4, 6/8, 2/4 ...

pub struct Session {
    pub id: SessionId,
    pub name: Name,
    pub program: u8,                  // SL MK3 の Session 番号 (0..=63)。キュー時は +64 して送る（§4.2）
    pub bars: u8,                     // 小節数 (1..=MAX_BARS_PER_SESSION)。0 は Bank の空きスロット
    pub meter: TimeSig,               // Session 既定の拍子
    pub bar_meter: [Option<TimeSig>; MAX_BARS_PER_SESSION], // 小節単位の上書き
}

pub struct Chapter {
    pub name: Name,
    pub sessions: FixedVec<SessionId, MAX_SESSIONS_PER_CHAPTER>, // 参照列
}

/// Song 内の絶対位置（arrangement 上の位置）
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct SongPos {
    pub arr_idx: u8,   // arrangement 内インデックス
    pub sess_idx: u8,  // その Chapter 内の Session インデックス
    pub bar: u8,       // その Session 内の小節
}

pub struct TempoTrigger { pub at: SongPos, pub bpm: u16 }

pub struct Song {
    pub name: Name,
    pub default_bpm: u16,
    pub chapters: FixedVec<Chapter, MAX_CHAPTERS_PER_SONG>,
    pub arrangement: FixedVec<ChapterIdx, MAX_ARRANGEMENT_LEN>,
    pub tempo_triggers: FixedVec<TempoTrigger, MAX_TEMPO_TRIGGERS>, // at 昇順
}

pub struct Bank {                       // 装置全体の保持データ
    pub sessions: [Session; MAX_SESSIONS],  // id を添字にする。bars == 0 が空き
    pub songs: FixedVec<Song, MAX_SONGS>,
}
```

実装上の決定（Phase 16。理由の詳細は `docs/results/phase16.md`「仕様からの逸脱」）:

- `heapless::Vec` の代わりに自前の `FixedVec<T, N>`（長さ `u8`）を使う。依存 crate を増やさないためと、ホストと wasm32 で `size_of` を一致させるため
- `Bank.sessions` は `[Option<Session>; N]` ではなく `[Session; N]`（`bars == 0` が空き）。`Option<Session>` は niche 最適化で None が非ゼロのビット列になり、`static` の Bank が .bss に落ちず `.wasm` を太らせるため。読み出しは `Bank::session(id) -> Option<&Session>`
- サイズ（wasm32 / ホスト共通）: `Session` 69 B、`Chapter` 33 B、`Song` 678 B、`Bank` 9,842 B、`Transport` 24 B

### 3.1 拍子とテンポの置き場所（設計判断）

| 属性 | 置き場所 | 理由 |
|---|---|---|
| 拍子・小節数 | Session（小節単位で上書き可） | SL MK3 のパターン長と対応する構造的属性。Session を使い回しても変わらない |
| テンポ | Song の TempoTrigger | Session をテンポの異なる曲で使い回せるようにする |

### 3.2 解決規則

- **有効テンポ** = 再生位置以前で最後の TempoTrigger の `bpm`。無ければ `Song.default_bpm`
- **有効拍子** = `Session.bar_meter[bar]` があればそれ、無ければ `Session.meter`
- Song の文脈が無い再生（Session 画面から単体再生）では、テンポは **Transport の現在テンポ**（前回使用値、初期値 120）を使う

Scope が途中の Session から始まっても、上の規則は「Song 先頭からその位置までを走査」して求めるため常に一意に決まる。

### 3.3 拡張のための予約

- `Session` に将来 `content: SessionContent`（ドラムパターン / コード進行 / 録音フレーズ）を追加する。v1 では持たない
- `Bar` レベルの上書きは拍子のみ。コードなど拍単位の情報は `content` 側に置く

---

## 4. Transport（再生機構）

装置に 1 つだけ存在する。画面はそれを覗く窓であり、画面遷移で再生は止まらない。

```rust
pub enum Scope {
    Song   { song: u8 },                              // arrangement 全体を 1 回
    Session { session: SessionId, single_bar: Option<u8>, repeat: bool },
}

pub struct Position { pub song_pos: Option<SongPos>, pub session: SessionId, pub bar: u8, pub beat: u8 }

pub enum State {
    Stopped,
    Playing { scope: Scope, pos: Position, queued: Option<QueuedAction> },
}

/// [Phase 16] 旧版の enum Transport を State とし、Transport の現在テンポ（§3.2）と現在の拍子を足した
pub struct Transport { state: State, bpm: u16, requested_bpm: Option<u16>, meter: TimeSig }

pub enum QueuedAction { Stop, Jump(SongPos), JumpBar(u8) }   // 次の小節境界で実行。JumpBar は Session scope 用 [Phase 16 で追加]
```

小節境界で `Transport::advance_bar()` を呼ぶと、境界で送るべきもの（Start / Stop / PC / テンポ / 拍子）が値（`BarEvents`）で返る。
Transport は `Copy` なので、複製して進めれば境界より前に次の小節の内容を知れる（PC の先行送信に使う）。

### 4.1 v1 の再生トグル（Session 画面のみ）

pptx の右上 2 トグルを Session 画面（Bar 一覧）にのみ実装する。

| `1` | 矢印 | 動作 |
|---|---|---|
| OFF | OFF | Session の全小節を再生して停止 |
| OFF | ON | Session の全小節を繰り返し |
| ON | OFF | 選択した小節だけ再生して停止 |
| ON | ON | 選択した小節だけ繰り返し（練習ループ） |

トグルは再生中にも変更でき、**次の小節境界から**反映する。
再生中に `1` を ON にした場合は、次の境界で選択小節へ移り、（矢印 OFF なら）それを弾き終えたら停止する `[Phase 16 で確定]`。
Song / Chapter 画面への横展開は後続 Phase とし、v1 の Song 再生は「arrangement 全体を 1 回再生して停止」に固定する。

### 4.2 境界イベントとMIDI出力

| タイミング | 動作 |
|---|---|
| Play 開始 | MIDI Start、（Song scope なら）先頭 Session の PC を **即時モード**（+64 なし）で Start の前に送信 |
| 拍頭 | メトロノーム click（1 拍目はアクセント） |
| 小節境界 | 有効拍子・有効テンポの再評価、QueuedAction の実行、トグル変更の反映 |
| Session 境界 | 次 Session の PC を **キューモード**（+64）で境界の `PC_LEAD_TICKS` 手前に送信。同じ Session が続けて参照されていても、arrangement 上の枠が変わるたびに送る |
| 停止 | MIDI Stop |

**SL MK3 の PC 仕様** `[Phase 16 で確定]`（Novation の公開仕様。ユーザー確認済み）:

- **ch16** の Program Change で Session を読み込む。Bank Select は不要
- 番号 0..=63 は **即時** に切り替わる。**+64 すると再生中パターンの末尾へキュー** される

**PC の送信先行量**: `PC_LEAD_TICKS = 24`（24ppqn = 4 分音符 1 つ） `[Phase 16 で確定・暫定]`。
キューモードでは切替タイミングを SL MK3 自身のパターン末尾が決めるので、送信側に要求されるのは「最後のパターン周回に入ってから、境界より前に届く」ことだけである。
4 分音符 1 つは、240bpm でも 250ms の余裕があり、パターン長が 4 分音符以上なら最後の周回に収まる。
PC は `seq_write(port=DIN_OUT)` で tick に予約するので、`app_tick` のジッタには依存しない。
（未検証の前提: SL MK3 のパターンが 4 分音符より短くないこと、トラックごとにパターン長が異なる場合の「末尾」の定義。Phase 19 の end-to-end で確認する）

### 4.3 タイミングの責務分担

既定の原則（`app_tick` ジッタ上限 5.1ms）に従う。

| 処理 | 担当 |
|---|---|
| 24ppqn MIDI Clock 生成 | native（既存スケジューラ） |
| メトロノーム click 発音 | native |
| 小節境界でのテンポ / 拍子切り替え | native（app から次の値を予約） |
| 拍 / 小節イベントの通知 | native → app |
| PC / Start / Stop の送信判断 | app（5ms ジッタで許容） |
| 曲構造の解決、Scope、トグル | app |

---

## 5. 画面

### 5.1 画面構成（スタック遷移 + パンくず）

2.8 インチでは 1 画面 1 階層とし、ヘッダにパンくず（例: `Hello > A > 1`）を出す。

```
Menu
 ├─ Song   → Song 一覧 → Chapter 一覧(arrangement) → Session 一覧(参照) → Session 画面(Bar 一覧)
 └─ Session → Session 一覧(グローバル) ──────────────────────────────→ Session 画面(Bar 一覧)
```

Session 画面は両ルートから **同じ画面** に到達する。

### 5.2 v1 で実装する画面

| 画面 | 内容 | Phase |
|---|---|---|
| Session 画面 | Bar 一覧、トグル `1` / 矢印、Play / Stop、再生中小節の点滅 | 18 |
| Session 一覧 | グローバル Session の一覧、選択で Session 画面へ | 18 |
| Song 一覧 / Chapter 一覧 | 選択と Song 再生開始、再生中 Chapter / Session の点滅 | 19 |
| Menu | Song / Session の入口 | 18 |

### 5.3 表示規則

- **再生中マーカー**（点滅）と **選択カーソル**（枠）は別物として描き分ける
- Bar 一覧は縦 1 列（1 行 1 小節）。表示は `小節番号 [拍子]` とし、拍子は Session 既定と異なる小節のみ表示
- 再生中の要素が現在の画面外にある場合（別 Song を閲覧中など）、ヘッダに小さく再生中インジケータを出す
- **タップ = 選択のみ**。再生位置のジャンプは別操作（長押し等）とし、次の小節境界でキュー実行する `[Phase で確定]`

---

## 6. Host API 要求（App-drives-API）

既存 API 名は Claude Code が `managed_components/` と Host API 定義を読んで確認すること。以下は要求であり名称は仮。

**Phase 16 の判定**（`[Phase 16 で確定]`、詳細は `docs/results/phase16.md`「Host API ギャップ分析」）を「既存 / 新規」列に反映した。

| # | 要求 | 既存 / 新規 | 備考 |
|---|---|---|---|
| H1 | テンポ設定（次の小節境界から有効） | **既存** `hostapi_tempomap_set_tempo(at_song_tick, upq)` | 未来の at_tick を指定すれば境界同期になる。**ただしマップは 32 件で、消す・クリアする語彙が無い**（H8） |
| H2 | 拍子設定（次の小節境界から有効） | **既存** `hostapi_tempomap_set_meter(at_song_tick, n, d)` | H1 と同じ制約 |
| H3 | 拍 / 小節イベントの取得（`app_tick` 内でポーリング可能な形） | **既存** `hostapi_transport_get_position` のポーリングで足りる | 表示は最大 1 app_tick 遅れる。発音・PC は tick 予約なので影響しない |
| H4 | MIDI Program Change 送信 | **既存** `hostapi_seq_write(port=DIN_OUT)`（境界前の予約）/ `hostapi_midi_send`（再生開始前の即時） | `transport_start` がキューを空にするので、開始時の PC は `midi_send` で先に送る |
| H5 | MIDI Start / Stop 送信 | **既存** `hostapi_transport_start` / `stop` | `hostapi_midi_send` で 0xFA / 0xFC を送ってはいけない（二重送出） |
| H6 | メトロノーム click ON/OFF、アクセント | **既存** `hostapi_seq_write(port=CLICK, OP_TONE)` + `hostapi_tone_define` | OFF は書かないだけ。アクセントは別スロット |
| H7 | Bank の永続化（read / write） | 新規 | v1 後続 Phase |
| H8 | テンポ / 拍子マップのリセット（再生開始時）と、長時間再生で枯渇しないこと | **新規** `[Phase 16 で追加]` | Phase 17。方式案は results |
| H9 | 小節境界ちょうどでの停止 | **新規（推奨）** `[Phase 16 で追加]` | Phase 17。回避策（`seq_write` で 0xFC を予約）はあるが推奨しない |

Host API / ABI の変更は承認ゲートを通す（既定の運用）。

---

## 7. 未決事項（Phase で確定）

| # | 項目 | 確定 Phase | 結果 |
|---|---|---|---|
| Q1 | SL MK3 の PC 反映タイミング → `PC_LEAD_TICKS` | 16 | **確定**: 0..=63 は即時、+64 でパターン末尾へキュー。Session 境界はキューモードで `PC_LEAD_TICKS = 24`（暫定） |
| Q2 | SL MK3 の PC 受信チャンネル、Bank Select の要否 | 16 | **確定**: ch16、Bank Select 不要 |
| Q3 | 上限定数の確定（メモリ見積もり） | 16 | **確定**: §3 の値のまま（Bank 9,842 B。linear memory は PSRAM 上で問題にならない） |
| Q4 | H1–H3 が既存 API で足りるか | 16（調査）/ 17（実装） | **調査済み**: H1–H6 は既存で足りる。H8（マップのリセット / 枯渇対策）と H9（境界同期の停止）が新規 |
| Q5 | ジャンプ操作の UI（長押し / ボタン） | 18 | |
| Q6 | カウントインを v1 に含めるか | 18 | |
| Q7 | Song / Chapter 画面へのトグル横展開の仕様（掘り下げ先の操作が再生 Scope に効くか） | 20 以降 | |
| Q8 | MIDI Clock の 115–119bpm 検出問題（別件）をどの Phase の前に解決するか | Roadmap 参照 | **クローズ**（2026-09-13。Phase 9c〜14 で解決済み、再発なし。roadmap U-1） |
