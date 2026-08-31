# Host API 仕様案 — 音楽時間軸 API(Phase 10 成果物 / **未承認ドラフト**)

対応指示書: `docs/prompts/phase10.md`。実測の出典: `docs/results/phase10.md`。
アーキテクチャ本体: `docs/architecture-next.md`。

**承認されたら、本文書の §7 のコード片を `shared/hostapi_defs.h` に取り込む。**
実装は Phase 11 以降で、本フェーズには含まない。

## 0. レビュー観点(最初に読むこと)

> **「この API 語彙は要件 1〜5 を通しても増えない」が層の切り方の検証である。**

本文書の §6「アプリ要件突き合わせ表」が、その検証結果である。
5 要件すべてを、下記 12 関数だけで実現できることを確認した。

## 1. 追加する語彙(全 12 関数)

既存 API と同じ規約に従う: エラーは負数(通常 -1)、アプリの不正入力でトラップさせない。
out-buffer は (ptr, len) を渡してホストが書いた件数/長さを返す。
構造体は固定サイズ・リトルエンディアン・ABI 凍結。

| グループ | 関数 | シグネチャ |
|---|---|---|
| transport | `hostapi_transport_start()` | `()i` |
| | `hostapi_transport_stop()` | `()i` |
| | `hostapi_transport_continue()` | `()i` |
| | `hostapi_transport_locate(tick)` | `(i)i` |
| | `hostapi_transport_get_position(buf_ptr, buf_len)` | `(*~)i` |
| tempomap | `hostapi_tempomap_set_tempo(at_tick, us_per_quarter)` | `(ii)i` |
| | `hostapi_tempomap_set_meter(at_tick, numer, denom)` | `(iii)i` |
| | `hostapi_tempomap_set_loop(start_tick, end_tick)` | `(ii)i` |
| seq | `hostapi_seq_write(buf_ptr, buf_len)` | `(*~)i` |
| | `hostapi_seq_flush_after(tick)` | `(i)i` |
| | `hostapi_seq_filled_until()` | `()i` |
| time | `hostapi_time_us_to_tick(us)` | `(I)i` |

## 2. tick の 2 つの座標(重要)

L0 のキューは **単調増加する playback tick** をソートキーにする(テンポ変更で
積み直しが不要になる設計の前提)。ループで巻き戻るのは **song tick** で、
その写像は L1 が持つ。

| 用語 | 意味 | 誰が使うか |
|---|---|---|
| **playback tick** | transport 開始からの単調増加 tick。ループしても戻らない | `seq_write` の `tick`、`seq_filled_until`、`time_us_to_tick` の戻り値、L0 のソートキー |
| **song tick** | 楽曲上の位置。ループ範囲の終端で先頭へ巻き戻る | `transport_locate` の引数、`tempomap_*` の `at_tick`、小節/拍の算出 |

`transport_get_position` は**両方を返す**。L2 は playback tick で先読みを供給し、
L3 は song tick で曲構造を解釈する。ループ 1 周分の内容は、L2 が周回ごとに
新しい playback tick で書き直す。

> レビュー論点: この切り方でよいか(`architecture-next.md` §11-1)。
> 代案は「tick をソング位置にしてループで巻き戻す」だが、L0 のソートキーが
> 単調でなくなり、キュー実装が複雑化する。

## 3. transport

```
hostapi_transport_start() -> 0/-1
  song tick 0 から再生を開始する。playback tick も 0 にリセット。
  - L0 のキュー、テンポマップの「現在位置」、MIDI クロックのグリッド位相を
    すべて 0 に揃える。
  - MIDI Start(0xFA)を DIN_OUT へ送出し、24ppqn クロックの生成を開始する
    (グリッドから直接生成。アプリはクロックを意識しない)。
  - 既に PLAYING のときは何もせず -1。

hostapi_transport_stop() -> 0/-1
  再生を停止する。
  - MIDI Stop(0xFC)を送出し、クロック生成を止める。
  - L0 のキューに残った未発火イベントは破棄する。
  - 発音中のノートに対する All Notes Off はホストが自動送出しない
    (アプリが必要に応じて hostapi_midi_send で送る。App drives の原則)。
  - 既に STOPPED のときは何もせず -1。

hostapi_transport_continue() -> 0/-1
  停止位置から再生を再開する。song tick は stop 時の値を保つ。
  playback tick は単調増加を維持するため、stop 時の値から続行する。
  - MIDI Continue(0xFB)を送出。
  - 既に PLAYING のときは -1。

hostapi_transport_locate(song_tick) -> 0/-1
  song 位置を移動する。
  - STOPPED 中: 次の start/continue の開始位置になる。
  - PLAYING 中: 即座にジャンプする。L0 の未発火イベントは破棄され
    (移動前の位置に対する予約なので)、L2 が新しい位置から供給し直す。
    アプリは locate 後に seq_filled_until() を見て再供給すること。
  - Song Position Pointer の送出は v1 ではスコープ外(9c からの持ち越し)。

hostapi_transport_get_position(buf_ptr, buf_len) -> 0/-1
  現在位置を hostapi_position_t(32 バイト)で返す。
  buf_len が 32 未満なら何も書かずに -1。
```

## 4. tempomap

テンポ・拍子は **song tick 上のマップ**として持つ。`at_tick` に指定した位置から
その値が有効になる(同じ at_tick への再設定は上書き)。

```
hostapi_tempomap_set_tempo(at_song_tick, us_per_quarter) -> 0/-1
  テンポを設定する。単位は「4 分音符あたりのマイクロ秒」(SMF の set tempo
  メタイベントと同じ単位。120bpm = 500000)。
  - 有効範囲は 20000..10000000(約 3000bpm..6bpm)。範囲外は -1。
  - at_song_tick == 0 かつ STOPPED なら初期テンポの設定。
  - PLAYING 中に「現在位置より過去」の at_tick を指定した場合は -1
    (既に通過した区間のテンポは変更できない)。
  - マップのエントリ数には上限がある(下記 §5 の実装メモ参照)。溢れたら -1。

hostapi_tempomap_set_meter(at_song_tick, numer, denom) -> 0/-1
  拍子を設定する。denom は 2 の冪(1/2/4/8/16)。
  - numer は 1..32、denom が 2 の冪でなければ -1。
  - 小節番号・拍番号(get_position の bar / beat)はこのマップから算出される。

hostapi_tempomap_set_loop(start_song_tick, end_song_tick) -> 0/-1
  ループ範囲を設定する。end に達した時点で song tick が start へ巻き戻る
  (playback tick は単調増加のまま。§2 参照)。
  - start >= end なら -1。
  - start == end == 0 でループ解除。
```

## 5. seq

L2 が先読みイベントを供給する経路。

```
hostapi_seq_write(buf_ptr, buf_len) -> n
  hostapi_seq_event_t の配列(buf_len はバイト数)を L0 のキューへ積む。
  受理した件数 n を返す(キューに空きがなければ要求より少ない。0 もありうる)。
  - tick は playback tick(絶対)。現在位置より過去の tick は可及的速やかに
    発火する(取りこぼしよりは遅延を選ぶ)。
  - 同一 tick 内の順序は、この関数に書かれた順を保つ。
  - buf_len / 16 件を上限に読む。端数バイトは無視。
  - キュー深さは 256 件(根拠は architecture-next.md §9)。
  - L2 は毎 tick 「filled_until() < now + horizon なら書き足す」を回すだけでよい。

hostapi_seq_flush_after(tick) -> n
  playback tick が指定値以上の未発火イベントをキューから取り除き、
  取り除いた件数を返す。パンチイン、曲の差し替え、locate 後の再供給に使う。
  - tick == 0 で全件破棄。

hostapi_seq_filled_until() -> tick
  キューに積まれている最後のイベントの playback tick を返す。
  キューが空なら現在の playback tick を返す(L2 が「ここから書けばよい」と
  解釈できる値にする)。STOPPED 中は 0 か locate 済み位置。
```

### 実装メモ(承認後の Phase 11 向け)

- L0 キュー: 静的 BSS 4KB = 256 件。tick 昇順の挿入ソート配列を推奨
  (L2 からはほぼ昇順に届くので実質末尾追記)。
- テンポマップ / 拍子マップ: 同じく静的確保。エントリ数は先読み horizon の
  範囲をカバーできればよく、32 件程度で足りる見込み(SMF インポート時に
  マップが長い曲は、L3 が horizon に合わせて逐次投入する)。
- MIDI クロックはグリッド(40 tick ごと)から L1 が生成し、**キューを消費しない**。

## 6. アプリ要件突き合わせ表(語彙の検証)

各要件を、上記 12 関数 + 既存 API だけで実現できるかを確認した。

### 要件 1: 高精度メトロノーム

| 実現内容 | 使う API |
|---|---|
| テンポ設定 | `tempomap_set_tempo(0, 500000)` |
| 拍子設定 | `tempomap_set_meter(0, 4, 4)` |
| クリック音の予約 | `seq_write([{tick:0, port:CLICK, status:OP_TONE, param:0}, {tick:960, ..., param:1}, ...])` |
| 開始/停止 | `transport_start()` / `transport_stop()` |
| MIDI Clock 出力 | **アプリは何もしない**(L1 がグリッドから生成) |
| テンポ変更(演奏中) | `tempomap_set_tempo(次の小節頭の song_tick, 新 upq)` |
| 現在の拍表示 | `transport_get_position()` の bar / beat |

**語彙の追加なし。** 9c の二重テンポ管理が消え、クロックはグリッド生成になる。

### 要件 2: 楽曲メトロノーム(セクション・途中テンポ変更・小節毎 PC)

| 実現内容 | 使う API |
|---|---|
| セクション構成(A メロ/B メロ/サビ) | **L3 の WASM 内で完結**(ホスト API 不要) |
| 途中テンポ変更 | `tempomap_set_tempo(at_song_tick, upq)` を horizon の範囲で順次投入 |
| 途中拍子変更 | `tempomap_set_meter(at_song_tick, n, d)` |
| 小節毎のプログラムチェンジ | `seq_write({tick:小節頭, port:DIN_OUT, status:0xC0\|ch, data1:pc})` |
| クリックの強拍/弱拍 | `seq_write` の `param` でトーンスロットを切り替え |

**語彙の追加なし。**

### 要件 3: SMF インポート

| 実現内容 | 使う API |
|---|---|
| SMF パース | **L3 の WASM 内**(ホスト API 不要) |
| 分解能変換 | PPQN 480 → 960 は ×2(無損失。architecture-next.md §4) |
| set tempo メタ | `tempomap_set_tempo(at_tick, upq)` — SMF と同じ µs/quarter 単位なので**変換不要** |
| time signature メタ | `tempomap_set_meter(at_tick, n, d)` |
| ノート・CC・PC | `seq_write` |
| 長い曲の供給 | horizon 分ずつ `seq_filled_until()` を見て逐次投入 |

**語彙の追加なし。** テンポ単位を SMF に合わせた(µs/quarter)ことがここで効く。

### 要件 4: 2trk シーケンサー + 録音

| 実現内容 | 使う API |
|---|---|
| 2 トラック再生 | `seq_write`(port / MIDI チャンネルでトラックを区別) |
| ループ再生 | `tempomap_set_loop(start, end)` |
| 録音(入力の取得) | `hostapi_midi_recv`(**既存 API**、生バイト + µs) |
| 録音打刻の tick 化 | `hostapi_time_us_to_tick(us)` |
| パンチイン / 差し替え | `seq_flush_after(tick)` → `seq_write` で新内容 |
| 頭出し | `transport_locate(song_tick)` |
| 録音中の位置表示 | `transport_get_position()` |

**語彙の追加なし。** `time_us_to_tick` と `seq_flush_after` はこの要件のためにある。
受信打刻の精度限界(最大 ≈1.3ms、architecture-next.md §8)はクオンタイズ設計で吸収する。

### 要件 5: ドラムマシン(内蔵音源 port)

| 実現内容 | 使う API |
|---|---|
| 内蔵音源への発音 | `seq_write({tick, port:SYNTH, status:0x99, data1:note, data2:vel})` |
| 外部音源との併用 | 同じ `seq_write` で `port` を変えるだけ |
| 音色設定 | 音源固有のパラメータは `hostapi_midi_send` 相当の CC/SysEx、または将来の音源 API |

**語彙の追加なし。出力先の追加が `port` の enum 値 1 個で済む**ことが、
このポート抽象を置いた理由である。

### 結論

**5 要件すべてを通しても API 語彙は増えなかった。** 層の切り方は妥当と判断する。

唯一、要件 5 の「音色設定」だけは将来の内蔵音源エンジン設計に依存して
語彙が増える可能性がある(本フェーズのスコープ外。port 抽象の設計のみが対象)。

## 7. 既存 API との関係

| 既存 API | 扱い | 理由 |
|---|---|---|
| `hostapi_midi_send` | **残す**。ただし **Start/Stop/Continue の副作用(内部クロック生成のトリガ)は削除する** | 素の MIDI バイト送出(SysEx、即時 CC、All Notes Off)は引き続き必要。一方、副作用によるテンポ逆算が 9c の根本原因を作ったので、transport_* に一本化する |
| `hostapi_midi_recv` | **残す**(シグネチャ・挙動とも不変) | パースはアプリの責務のまま。打刻補正を入れるかは要判断(architecture-next.md §11-4) |
| `hostapi_click_schedule` | **非推奨化**(当面は残す) | `seq_write(port=CLICK)` に置換。既存アプリ(metronome / clicktest)の回帰を守るため、移行ステップ 4 で内部を L0 の薄いラッパに載せ替える |
| `hostapi_tone_schedule` | **非推奨化**(同上) | 同上 |
| `hostapi_tone_define` / `hostapi_tone_play` | **残す** | トーンパレットの定義・即時発音は L0 の CLICK port が使う。予約だけが seq に移る |
| `hostapi_now_ms` | **残す** | UI 用の実時間。音楽時間軸とは別系統 |
| gfx / input / audio / fs | **変更なし** | 本改訂の対象外 |

**ABI の非破壊性**: 追加は新シンボルのみ。既存シンボルのシグネチャは変更しない。
`hostapi_midi_send` の副作用削除は挙動変更なので、移行ステップ 5 で
midi_loopback による確認を伴って行う。

## 8. shared/hostapi_defs.h への追加案(承認後に取り込むコード片)

```c
/* ============================== transport / tempomap / seq ==============================
 *
 * 音楽時間軸 API(Phase 11 以降)。設計と根拠は docs/architecture-next.md 参照。
 *
 * tick の 2 座標:
 *   playback tick = transport 開始からの単調増加。ループしても戻らない。
 *                   seq_write の tick、seq_filled_until、time_us_to_tick の戻り値。
 *   song tick     = 楽曲上の位置。ループ範囲の終端で先頭へ巻き戻る。
 *                   transport_locate の引数、tempomap_* の at_tick、小節/拍の算出。
 *
 * 内部 PPQN は 960(24 で割り切れ MIDI Clock が整数 40 tick、SMF 最頻 480 の ×2)。
 * MIDI Clock はホストが 40 tick グリッドから直接生成する。アプリは関与しない。
 */

#define HOSTAPI_PPQN 960

/* transport の状態 */
enum {
    HOSTAPI_TRANSPORT_STOPPED = 0,
    HOSTAPI_TRANSPORT_PLAYING = 1,
};

/* イベントの出力先ポート */
enum {
    HOSTAPI_PORT_DIN_OUT  = 0, /* 物理 MIDI OUT (UART1) */
    HOSTAPI_PORT_USB_MIDI = 1, /* 将来 */
    HOSTAPI_PORT_SYNTH    = 2, /* 内蔵音源(将来) */
    HOSTAPI_PORT_CLICK    = 3, /* トーンパレット(hostapi_tone_define のスロット) */
};

/* status が MIDI ステータスバイト(0x80 以上)でない場合の内部オペコード */
enum {
    HOSTAPI_SEQ_OP_NONE = 0,
    HOSTAPI_SEQ_OP_TONE = 1, /* port=CLICK。param = トーンスロット (0..7) */
    /* 将来: OP_MARKER, OP_CALLBACK, ... 追加は非破壊 */
};

/* シーケンサイベント。16 bytes, align 4。リトルエンディアン(ABI 凍結)。
 * L0 の内部キュー要素と同一レイアウトで、境界での変換を不要にしている。 */
typedef struct {
    uint32_t tick;      /* 発火する playback tick(絶対) */
    uint8_t  port;      /* HOSTAPI_PORT_* */
    uint8_t  status;    /* MIDI ステータスバイト、または HOSTAPI_SEQ_OP_* */
    uint8_t  data1;     /* MIDI データ 1(未使用なら 0) */
    uint8_t  data2;     /* MIDI データ 2(未使用なら 0) */
    uint32_t param;     /* op 依存。MIDI イベントでは 0 */
    uint32_t _reserved; /* 常に 0。将来拡張用でサイズ変更はしない */
} hostapi_seq_event_t;

/* transport 位置。32 bytes, align 8。リトルエンディアン(ABI 凍結)。 */
typedef struct {
    uint64_t host_us;      /* この位置に対応するホスト時刻(µs、単調増加) */
    uint32_t tick;         /* playback tick(単調増加) */
    uint32_t song_tick;    /* song tick(ループで巻き戻る) */
    uint32_t bar;          /* song_tick 基準の小節番号(0 始まり) */
    uint32_t tempo_upq;    /* 現在有効なテンポ(µs / 4 分音符) */
    uint16_t beat;         /* 小節内の拍(0 始まり) */
    uint16_t tick_in_beat; /* 拍内 tick */
    uint32_t state;        /* HOSTAPI_TRANSPORT_* */
} hostapi_position_t;

/* HOSTAPI_NATIVE_SYMBOLS(X) へ追記する分 */
    /* transport / tempomap / seq (Phase 11) */             \
    X(hostapi_transport_start, "()i")                       \
    X(hostapi_transport_stop, "()i")                        \
    X(hostapi_transport_continue, "()i")                    \
    X(hostapi_transport_locate, "(i)i")                     \
    X(hostapi_transport_get_position, "(*~)i")              \
    X(hostapi_tempomap_set_tempo, "(ii)i")                  \
    X(hostapi_tempomap_set_meter, "(iii)i")                 \
    X(hostapi_tempomap_set_loop, "(ii)i")                   \
    X(hostapi_seq_write, "(*~)i")                           \
    X(hostapi_seq_flush_after, "(i)i")                      \
    X(hostapi_seq_filled_until, "()i")                      \
    X(hostapi_time_us_to_tick, "(I)i")
```

## 9. time_us_to_tick

```
hostapi_time_us_to_tick(us) -> playback_tick   /* us は i64 */
  ホスト時刻(µs、hostapi_midi_recv のタイムスタンプと同一時基)を
  playback tick に変換する。録音の打刻に使う。
  - STOPPED 中は -1(時間軸が動いていないため変換できない)。
  - 現在位置より未来の us も変換できる(テンポマップが確定している範囲まで)。
  - 精度: 変換自体は Clock Authority 経由で ±62µs(P10-1)。ただし入力となる
    hostapi_midi_recv のタイムスタンプ自体が、連続受信時に最大 ≈1.3ms の
    バッチング誤差を持つ(P10-5)。録音のクオンタイズ設計ではこちらが支配的。
```

## 10. アプリ側の典型的なループ(L2 の実装イメージ)

```rust
// app_tick() 内(100ms 周期)
let mut pos = [0u8; 32];
hostapi_transport_get_position(pos.as_mut_ptr(), 32);
let now_tick = u32::from_le_bytes(pos[8..12].try_into().unwrap());

let horizon = 2 * 4 * HOSTAPI_PPQN;           // 2 小節(4/4)
while hostapi_seq_filled_until() < now_tick + horizon {
    let events = build_next_chunk();           // L3 の曲構造から生成
    let n = hostapi_seq_write(events.as_ptr(), events.len() * 16);
    if n == 0 { break; }                       // キュー満杯。次の tick で再試行
}
```

アプリは**実時間を一切扱わない**。tick だけで先読みを供給し、
実時間への写像はホスト(L1/L0)が持つ。これが本改訂の要点である。
