# Sequencer App — 要求・仕様 (v0.1 draft)

- 対象: MidiAppBox 上で動作する最初の本格アプリ「Sequencer」
- 状態: 設計ドラフト。Phase 16 以降で確定する項目は `[Phase で確定]` と明記する
- 元資料: `MetronomeAppSpec.pptx`（画面ラフ）、2026-09-13 のレビュー議論

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

`no_std` / 固定長を前提とした Rust 表現。上限値は暫定であり Phase 16 でメモリ見積もりと合わせて確定する。

```rust
pub type SessionId = u8;   // 0..=MAX_SESSIONS-1
pub type ChapterIdx = u8;  // Song 内インデックス

pub const MAX_SESSIONS: usize = 64;          // SL MK3 の Session 数に合わせる [Phase で確定]
pub const MAX_BARS_PER_SESSION: usize = 16;
pub const MAX_CHAPTERS_PER_SONG: usize = 16;
pub const MAX_ARRANGEMENT_LEN: usize = 32;
pub const MAX_TEMPO_TRIGGERS: usize = 16;
pub const MAX_SONGS: usize = 8;

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct TimeSig { pub num: u8, pub den: u8 }   // 4/4, 3/4, 6/8, 2/4 ...

pub struct Session {
    pub id: SessionId,
    pub name: Name,                   // 固定長文字列 [Phase で確定]
    pub program: u8,                  // 送信する PC 番号 (0..=127)
    pub bars: u8,                     // 小節数 (1..=MAX_BARS_PER_SESSION)
    pub meter: TimeSig,               // Session 既定の拍子
    pub bar_meter: [Option<TimeSig>; MAX_BARS_PER_SESSION], // 小節単位の上書き
}

pub struct Chapter {
    pub name: Name,
    pub sessions: heapless::Vec<SessionId, MAX_BARS_PER_SESSION>, // 参照列
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
    pub chapters: heapless::Vec<Chapter, MAX_CHAPTERS_PER_SONG>,
    pub arrangement: heapless::Vec<ChapterIdx, MAX_ARRANGEMENT_LEN>,
    pub tempo_triggers: heapless::Vec<TempoTrigger, MAX_TEMPO_TRIGGERS>, // at 昇順
}

pub struct Bank {                       // 装置全体の保持データ
    pub sessions: [Option<Session>; MAX_SESSIONS],
    pub songs: heapless::Vec<Song, MAX_SONGS>,
}
```

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

pub enum Transport {
    Stopped,
    Playing { scope: Scope, pos: Position, queued: Option<QueuedAction> },
}

pub enum QueuedAction { Stop, Jump(SongPos) }   // 次の小節境界で実行
```

### 4.1 v1 の再生トグル（Session 画面のみ）

pptx の右上 2 トグルを Session 画面（Bar 一覧）にのみ実装する。

| `1` | 矢印 | 動作 |
|---|---|---|
| OFF | OFF | Session の全小節を再生して停止 |
| OFF | ON | Session の全小節を繰り返し |
| ON | OFF | 選択した小節だけ再生して停止 |
| ON | ON | 選択した小節だけ繰り返し（練習ループ） |

トグルは再生中にも変更でき、**次の小節境界から**反映する。
Song / Chapter 画面への横展開は後続 Phase とし、v1 の Song 再生は「arrangement 全体を 1 回再生して停止」に固定する。

### 4.2 境界イベントとMIDI出力

| タイミング | 動作 |
|---|---|
| Play 開始 | MIDI Start、（Song scope なら）先頭 Session の PC を送信 |
| 拍頭 | メトロノーム click（1 拍目はアクセント） |
| 小節境界 | 有効拍子・有効テンポの再評価、QueuedAction の実行、トグル変更の反映 |
| Session 境界 | 次 Session の PC を送信 |
| 停止 | MIDI Stop |

**PC の送信先行量**: SL MK3 が PC を即時に反映するか、パターン末尾で反映するかに依存する。
`PC_LEAD_TICKS`（24ppqn 単位）を定数として持ち、値は **Phase 16 の実機実験で確定** する `[Phase で確定]`。

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

| # | 要求 | 既存 / 新規 | 備考 |
|---|---|---|---|
| H1 | テンポ設定（次の小節境界から有効） | 既存の拡張？ | 現行が即時反映なら境界同期の追加が必要 |
| H2 | 拍子設定（次の小節境界から有効） | 新規の可能性 | click のアクセント位置に影響 |
| H3 | 拍 / 小節イベントの取得（`app_tick` 内でポーリング可能な形） | 新規の可能性 | 位置 (bar, beat) と発生時刻 |
| H4 | MIDI Program Change 送信 | 既存（生バイト送信） | 3 バイト送信で足りる想定 |
| H5 | MIDI Start / Stop 送信 | 既存（Phase 9a） | |
| H6 | メトロノーム click ON/OFF、アクセント | 既存の拡張？ | |
| H7 | Bank の永続化（read / write） | 新規 | v1 後続 Phase |

Host API / ABI の変更は承認ゲートを通す（既定の運用）。

---

## 7. 未決事項（Phase で確定）

| # | 項目 | 確定 Phase |
|---|---|---|
| Q1 | SL MK3 の PC 反映タイミング → `PC_LEAD_TICKS` | 16 |
| Q2 | SL MK3 の PC 受信チャンネル、Bank Select の要否 | 16 |
| Q3 | 上限定数の確定（メモリ見積もり） | 16 |
| Q4 | H1–H3 が既存 API で足りるか | 16（調査）/ 17（実装） |
| Q5 | ジャンプ操作の UI（長押し / ボタン） | 18 |
| Q6 | カウントインを v1 に含めるか | 18 |
| Q7 | Song / Chapter 画面へのトグル横展開の仕様（掘り下げ先の操作が再生 Scope に効くか） | 20 以降 |
| Q8 | MIDI Clock の 115–119bpm 検出問題（別件）をどの Phase の前に解決するか | Roadmap 参照 |
