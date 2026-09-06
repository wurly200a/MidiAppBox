# Phase 13 実施記録 — metronome を新 API で書き直す(移行ステップ 3)

対応する指示書: `docs/prompts/phase13.md`
関連: `docs/architecture.md`(§5〜§7 / §10 / §11)、`docs/hostapi.md`、
`docs/results/phase11.md`(ステップ 1〜2 の実装)、`docs/results/phase12.md`(申し送り)。

## セッション冒頭の環境確認(2026-09-06)

| 項目 | 結果 |
|---|---|
| `hpane.sh ensure unix-build` の冪等性 | 同一 pane_id(2 回とも `wC:pJ`) |
| `hpane.sh run unix-build "echo hello"` | exit 0 |
| ALSA シーケンサ | **使える**。`/dev/snd/seq` は ACL 付き(`crw-rw----+`)でアクセス可 |
| UM-ONE | `aconnect -l` に `client 20: 'UM-ONE' [type=kernel,card=1]`、`lsusb` に `0582:012a Roland Corp. UM-ONE` |

→ Phase 12 の申し送り 2(「現在 Linux で ALSA が使えない」)は**解消済み**。
測定(実機 MIDI OUT → UM-ONE → PC)の前提が整っている。

## ステップ 1: 設計メモ(承認待ち)

### 1-0. 実装前に確認した既存実装の事実(`shared/seq_core.c`)

設計判断の前提になるので、仕様書の記述と実装を突き合わせた結果を先に置く。

| 確認項目 | 実装の事実 | 出典 |
|---|---|---|
| 同一 `at_tick` への `set_tempo` 再設定 | **上書き**(エントリ数は増えない) | `seqcore_tempomap_set_tempo` の `at_tick == ` 分岐 |
| PLAYING 中に過去の `at_tick` を指定 | **-1**(受け付けない) | 同上、`at_song_tick < cur_song` |
| `at_tick == 現在位置` の `set_tempo` | 受理され、**その場で再アンカー**(`s_seg_*` を now で張り直す) | 同上 `need_reanchor` |
| テンポ/拍子マップの上限 | `SEQCORE_TEMPO_MAX = 32` / `SEQCORE_METER_MAX = 32`。**エントリを削除する API はない**(溢れたら -1) | `shared/seq_core.h` |
| `transport_locate`(PLAYING 中) | playback tick は維持、song tick のみ移動。キューは破棄。**MIDI は何も送らず、クロックグリッド `s_next_clock_pb`(playback tick 基準)にも触らない** | `seqcore_transport_locate` |
| CLICK ポートの発火 | `hooks->click(param)` → 実機 `audio::Play_Tone`、Linux `tone_play_impl`。`param` = トーンスロット | `src/components/seq/seq.cpp` / `hosts/linux/hostapi_seq.c` |
| 再アンカーの丸め | `cur_pb_locked()` は整数除算で切り捨てるため、再アンカーのたびに時間軸が**最大 1 tick(120bpm で 520µs)後ろへずれる** | `cur_pb_locked` / `host_us_of_locked` |

### 1-1. テンポの表現

- `upq = 60_000_000 / bpm`(四捨五入: `(60_000_000 + bpm/2) / bpm`)を
  `tempomap_set_tempo(0, upq)` で与える。
- 端数(132bpm → 454545.45… → 454545)はアプリ側で丸める。L1 の tick→µs 変換は
  アンカーからの絶対計算(`s_seg_us + d * upq / 960`)なので、丸め残差は
  **蓄積しない**(1 拍あたり最大 0.5µs の定常誤差に留まる)。
- 120bpm は割り切れて 500000。クロック間隔は `40 * 500000 / 960 = 20833.33µs` で、
  絶対計算+切り捨てにより個々の間隔は 20833 / 20834 を取り、平均は 20833.33µs。
  目標「平均 20833µs ± 10µs」を満たす。

### 1-2. 演奏中のテンポ変更 — **「次の小節頭」ではなく「即時(locate(0) + set_tempo(0))」を提案**

指示書の案は「次の小節頭の song_tick に `set_tempo` を投入する」だが、
**採らないことを提案する**。理由は 2 つ:

1. **テンポマップが溢れる。** 異なる `at_tick` への `set_tempo` はエントリを 1 つ
   消費し、削除する API がない。`SEQCORE_TEMPO_MAX = 32` なので、**32 個の異なる
   小節頭でテンポを変えた時点で以後の変更が -1 になり、メトロノームとして壊れる**
   (120bpm 4/4 なら 1 小節 2 秒。5 分の測定でも 150 小節ある)。
2. **長押し連打(100ms 間隔)と両立しない。** 小節頭適用だと 1 小節に 1 回しか
   音が変わらず、画面の BPM 表示だけが先行する。旧版(Phase 7D)は連打の
   1 ステップごとに即座にテンポが変わるので、**機能の維持に反する**。

提案する方式(旧版の `rearm(now)` と同じ意味論):

```
STOPPED 中:  tempomap_set_tempo(0, upq)                    // エントリは常に 1 個(上書き)
PLAYING 中:  transport_locate(0)                            // song を 0(= 小節頭)へ。キュー破棄
             tempomap_set_tempo(0, upq)                     // cur_song == 0 なので受理・上書き
             resync()                                       // PENDING 破棄 → 拍 0 から供給し直す
```

- **メトロノームの「曲」は 1 小節の繰り返しであり、設定変更のたびに位置 0 から
  やり直す**、というモデル。`set_loop` は使わない(指示書どおり)。
- テンポマップは **常に at_tick=0 の 1 エントリだけ**。上書きなので溢れない。
- `transport_locate` は MIDI を何も送らず、クロックグリッドにも触らないので、
  **0xFA/0xFC は START/STOP の 1/1 のまま**、クロックの欠落・二重も起きない。
- 旧版と同じく、変更の瞬間に小節頭のアクセント音が鳴る(旧版 `schedule_beat(0)` を
  `now` に予約するのと同じ挙動)。
- 長押し連打で 1 小節内に複数回来ても、毎回 locate(0)+上書きなので**最後の値だけが
  有効**になる。
- **代償(実測で確認する)**: 再アンカー 2 回ぶん(locate と set_tempo)の切り捨てで、
  変更 1 回につき時間軸が最大 ~1ms 後ろへずれる。これは**位相のオフセットであって
  蓄積ジッタではなく**、外れ値判定(公称の 1.5 倍 = 31250µs)には遠く届かない。
  条件 B は「テンポ変更区間を判定から除外し、120 に戻した後で判定」なので目標にも
  影響しない。

### 1-3. 拍子変更 — 演奏中も可(テンポと同じ即時方式)

旧版は演奏中でも BEAT ボタンで 2→3→4→6 を巡回し、`rearm(now)` で小節を
やり直していた。**旧挙動に合わせて演奏中も許可**し、同じ即時方式にする:

```
STOPPED 中:  tempomap_set_meter(0, numer, 4)
PLAYING 中:  tempomap_set_meter(0, numer, 4) → transport_locate(0) → resync()
```

拍子マップも at_tick=0 の 1 エントリ固定(上書き)。`denom` は常に 4。

### 1-4. クリックの供給(L2)

- イベントは `port = HOSTAPI_PORT_CLICK` / `status = HOSTAPI_SEQ_OP_TONE` /
  `param = スロット`。小節頭(`i % numer == 0`)は `ACCENT_SLOT = 1`、他は `0`。
- **playback tick の算出**: `transport_get_position()` から
  `OFFSET = pos.tick - pos.song_tick`(locate/start の間は不変)を取り、
  拍 `i` の playback tick を `OFFSET + i * 960` とする。song tick = `i * 960` なので、
  ホストが返す `bar` / `beat` と自前のアクセント判定が構造的に一致する。
- horizon は **2 小節**(`numer * 960 * 2`)。4/4 120bpm で 8 拍 = 4 秒、
  キュー 256 件に対して十分小さい。
- 供給ループは `docs/hostapi.md` §10 / `seq_smoke` と同じプレフィックス受理契約の形
  (`PENDING` / `PENDING_LEN` / `PENDING_OFF`、`n < remain` なら次 tick で再送)。
- `transport_start` / `transport_stop` / `transport_locate` の直後は
  `PENDING` を破棄し、`NEXT_BEAT = 0`、`OFFSET` を取り直して供給し直す
  (§5 の破棄契約)。`seq_flush_after` は使わない(locate がキューを破棄するため)。

### 1-5. 拍ランプ

- `transport_get_position()` の `beat`(小節内の拍、0 始まり)で点灯位置を決め、
  `bar`/`beat` が変化した tick でだけ再描画する。1 拍目はアクセント色。
- app_tick は 100ms 周期なので**表示は最大 100ms 遅れる**。旧版と同じ制約で、
  音のタイミングには無関係。
- STOP 時は全消灯(旧版と同じ)。

### 1-6. START / STOP

- START = `transport_start()`(pb/song/クロックグリッドを 0 に揃え、0xFA を送出)。
  直後に `PENDING` 破棄・`NEXT_BEAT = 0`・`OFFSET = 0` として拍 0 から供給。
- STOP = `transport_stop()`(0xFC、キュー破棄、クロック停止)。`PENDING` 破棄。
- **`hostapi_midi_send` は呼ばない**(呼ぶとクロックが二重に出る)。
- CLICK のトーンは固定長ワンショットなので鳴りっぱなしは起きない(§11-8 の影響なし)。

### 1-7. 音量

`hostapi_audio_set_volume`(既存 API、変更なし)。V−/V+ は 10 刻み、0..100。

### 1-8. 旧経路を使っていないことの確認方法

1. **静的(主)**: `extern` 宣言から `hostapi_click_schedule` / `hostapi_tone_schedule` /
   `hostapi_midi_send` を削除するので、`.wasm` の import セクションにこれらの名前が
   現れない。ビルド後に `strings metronome.wasm | grep -E 'click_schedule|tone_schedule|midi_send'`
   が空であることを記録する(検証専用コード不要)。
2. **動的(従)**: 実機側の旧クロック生成は `hostapi_midi_send` の 0xFA でしか
   起動しない。測定で **0xFA が 1 回・0xF8 が期待数ちょうど**なら、旧経路が
   走っていないことの実測的な裏付けになる(二重なら 0xFA が 2 回、または
   クロック数が倍近くになる)。
3. 上記で疑義が残った場合にのみ、`PHASE13_OLDPATH_TEST` で
   `Midi_NotifyBeatScheduled/Fired` に一時ログを入れて確認し、検証後に削除する。

### 1-9. 旧版から維持する機能(対応表)

| 旧版の機能 | 新実装 |
|---|---|
| BPM 40〜240、±5(BPM−/BPM+)・±1(−1/+1) | 同じ。変更時に上記 1-2 の即時方式 |
| 長押し連打加速(500ms 後に開始、400→200→100ms) | 状態機械はそのまま流用(app_tick 内で完結) |
| 拍子 2/3/4/6(BEAT ボタンで巡回) | `tempomap_set_meter(0, n, 4)` + 1-3 |
| START / STOP | `transport_start` / `transport_stop` |
| 拍ランプ(1 拍目はアクセント色) | `get_position` の bar/beat から |
| アクセント音(1568Hz / slot 1) | `tone_define` は不変、`seq_write` の `param` で切替 |
| 音量 V− / V+ | `hostapi_audio_set_volume`(不変) |
| MIDI Start/Stop の送出 | **ホストが transport_start/stop で送る**(アプリは関与しない) |

### 1-10. 設計上の発見(Phase 10 の設計検証としての記録)

ゲート 5 の趣旨に沿って、仕様どおりに書こうとして引っかかった点を記録する。
**いずれも既存 API の範囲で回避できており、API/ABI の変更は要らない。**

1. **テンポ/拍子マップにエントリを削除・剪定する語彙がない。** 無限に続く
   (= 曲の終わりがない)アプリで「演奏中に何度もテンポを変える」と、異なる
   `at_tick` を使う限りマップは単調に増えて 32 で頭打ちになる。今回は
   「位置 0 へ locate して 1 エントリを上書きし続ける」で回避したが、
   要件 2(楽曲メトロノーム)や要件 3(SMF)を長時間ループ再生する段階で
   同じ問題が再燃しうる。
2. **ループ中は「現在位置より後ろのエントリ」を書き換えられない。**
   `set_loop` を使う場合、次の周回で通る位置でも `at_tick < cur_song` なら
   `set_tempo` は -1 になる。今回はループを使わないので影響なし。
3. **再アンカーは整数切り捨てで時間軸を最大 1 tick 後退させる。**
   仕様書には書かれていない実装上の性質。頻繁な再アンカーを行う設計では
   位相が後ろへずれていく(蓄積ジッタではない)。

**ここで停止 → 承認待ち。**

---

## ステップ 2: 測定ツール(実装・妥当性確認まで完了)

承認された方式(C の受信プローブ + Python の集計 + ラッパ)で実装した。

| ファイル | 役割 |
|---|---|
| `tools/midi_clock_probe/midi_clock_probe.c` | ALSA シーケンサから System Realtime を受け、1 件 1 行の CSV を吐く受信専用プローブ |
| `tools/midi_clock_probe/analyze.py` | 集計(E1 と同項目)。Markdown 表 + テキストヒストグラム |
| `scripts/midi-clock-probe.sh` | 計測 / 再集計 / TX ログ集計のラッパ。出力は `captures/<task>/` |

打刻は **ALSA のカーネル側打刻(real-time キュー)を主**、受信ループの
`CLOCK_MONOTONIC` を副として両方 CSV に残す。両者の差(表の「カーネル打刻と
ユーザ打刻の差」)がツール自身の遅延の自己検証になる。

### 実装中に踏んだ点(記録)

1. **`snd_seq_open` を `SND_SEQ_OPEN_INPUT` で開くとキューが走らない。**
   キュー開始イベントの送出に出力側が要るため、打刻が全件 0 になった。
   `SND_SEQ_OPEN_DUPLEX` で解決。
2. **STOPPED をまたぐ間隔はクロックの欠落ではない。** 0xFA/0xFB〜0xFC の
   「再生区間」を切り出し、区間内で連続するクロックだけで間隔統計を作るようにした
   (これをしないと stop→continue の空白が外れ値として数えられる)。
3. **`FILE*` の既定バッファのままだと計測中に外から CSV を読めない**
   (数十秒ぶん見えない)。定期 `fflush` を入れた。これが原因で、計測を
   案内する補助スクリプトが「STOP を検出できない」状態になった(下記 A3)。

### 妥当性確認

**(a) 合成データ**(既知の欠落を 2 件仕込んだ CSV): 外れ値 2 件・公称比 2.01x/2.02x を
正しく検出。120→180→120 の合成データで区間分割が 3 区間・平均間隔 20833/13889/20826 を
正しく復元。

**(b) Linux ホストの seq_smoke**(ALSA 直結、USB を通らない経路):

| 区間 | クロック数 | 平均間隔 | 見かけ BPM | 外れ値 |
|---|---|---|---|---|
| 120bpm | 283 | 20833.6 µs | 120.00 | 0 |
| 180bpm | 519 | 13889 µs(STOPPED 区間を除く) | 180.0 | 0 |

**Phase 11 の実測(120bpm: 20833.1µs / 180bpm: 13888.9µs)と一致**した。

**(c) 実機の seq_smoke**(実機 MIDI OUT → UM-ONE → PC。本フェーズの測定経路そのもの):

| 項目 | 結果 |
|---|---|
| 0xFA / 0xFB / 0xFC | **1 / 1 / 2**(seq_smoke の start → stop → continue → stop と一致) |
| 0xF8 総数 | 800 |
| 外れ値 | **0 件** |
| 120bpm 区間 | 288 発、平均 20826.7 µs |
| 180bpm 区間 | 510 発、平均 13888.4 µs |
| カーネル打刻とユーザ打刻の差 | mean 72 µs / max 1144 µs |

→ **ツールは欠落を作っていない**(USB を通しても 800 発で外れ値 0)。生データは
`captures/phase13/dev_seq_smoke.csv`。

## ステップ 3: 実装(完了)

`wasm-apps/metronome/src/lib.rs` を設計メモどおりに上書きで書き直した。

- `extern` から `hostapi_click_schedule` / `hostapi_tone_schedule` / `hostapi_midi_send` を削除。
  ビルド後の `.wasm` の import は 13 個で、**旧経路の 3 関数は含まれない**
  (`strings metronome.wasm | grep hostapi_` で確認)。
- `.wasm` サイズ: **2,885 B → 3,910 B(+1,025 B)**。ファーム全体は
  `0x1000f0` バイト、アプリパーティション残 75%(3,145,488 B)。
- Linux ホスト: `app_init=0` / `app started` / `app stopped`、警告 0。
  START/STOP をクリックして **0xFA=1 / 0xFC=1、1444 発・外れ値 0・平均 20833.4µs**
  (`captures/phase13/linux_metronome.csv`)。
- 実機: 手動確認(カメラ録画 `captures/phase13/cam_rec_142051.mp4`、静止画
  `cam_still_142254.png`)で START/STOP・BPM ±1/±5・長押し連打・拍子巡回・
  アクセント音・音量・拍ランプの全機能をユーザーが確認。異常報告なし。

### トラブル: SD 上の `.wasm` が壊れた(重要)

新しい `metronome.wasm` を焼いた直後、実機で
`WASM module load failed: magic header not detected` が続いた。

- ファーム内の埋め込みは正常(`src/build/midi_app_box.bin` 内に新 `.wasm` の
  バイト列が完全一致で存在、旧版は不在)。
- 起動時のシードは毎回 `seed: wrote /sdcard/apps/metronome.wasm (3910/3910 bytes)` を
  出す(= 書き込み成功と報告)のに、**次の起動でも内容不一致と判定されて再度書き込まれる**
  (= 書き込みが永続していない)。ソフトリセットでは直らなかった。
- SD を PC に挿して確認したところ、**サイズ 3910 B・先頭 3585 B がゼロ・末尾 217 B だけ非ゼロ**。
  ディレクトリエントリのサイズだけ正しく、データが載っていない状態だった。
- `fsck.vfat -n` の指摘は**ボリュームラベルが FAT として不正**という 1 点のみで、
  クラスタのロスト・クロスリンクは無し。**FS 構造は健全**だった。
- PC から同じファイルを上書きコピー → `sync` → アンマウント/再マウント後の
  読み直しで md5 一致を確認 → カードを戻して電源投入で解決。以後は起動時の
  シードも走らない(内容一致)。

→ 教訓: **シードの書き込み成功ログは、データが載ったことを保証しない。**
`magic header not detected` が出たら、SD を PC に挿して実体を確認するのが最短。

## ステップ 4: 測定(条件 A 実施中)

測定条件: 120bpm・4/4、実機 MIDI OUT → UM-ONE → PC。

### 条件 A(アイドル)

| # | 区間長 | 0xF8 | 期待数 | clocks/exp | 平均間隔 | 外れ値 | 見かけ BPM | 0xFA/0xFC |
|---|---|---|---|---|---|---|---|---|
| **A1** | 339.3 s | **16,287** | 16,287.0 | **100.00 %** | **20832.8 µs** | **0 件** | 単峰 (119.20〜120.85) | 1 / 1 |
| **A2** | 338.1 s | **16,229** | 16,229.5 | **100.00 %** | **20832.9 µs** | 2 件(下記) | 単峰 (113.40〜127.38) | 1 / 1 |
| A3 | — | 未実施(補助スクリプトの不具合で中断。ステップ 2 の記録 3) | | | | | | |

**A1 は絶対値目標を全項目クリア**(外れ値 0 / clocks÷expected 100.00% / 単峰 /
平均 20832.8µs / 0xFA・0xFC が 1・1)。

**A2 の外れ値 2 件について(未決着。条件 D で判定する)**

```
| # | 発生時刻 (s) | 間隔 (µs) | 公称比 |
| 16114 | 341.093 | 50072 | 2.40x |
| 16115 | 341.093 |     3 | 0.00x |
```

同一時刻に「50,072µs の空白 → 3µs 間隔で 2 発」というペアで出ている。
**MIDI DIN は 31250bps = 1 バイト 320µs なので、3µs 間隔の受信は物理的に
ありえない**(P10-5 と同じ理屈)。したがってこれは配送側(USB/ALSA)で
2 発がまとめて届いた受信アーティファクトの疑いが濃い。傍証:

- クロック総数は期待数と一致(**1 発も欠落していない**)。
- 同じ区間で「カーネル打刻とユーザ打刻の差」の最大が 3,303µs に跳ねている
  (A1 は 2,105µs)。ホスト側で配送が詰まった時刻と符合する。

ただし**現時点では実測として「外れ値 2 件」であり、目標未達の回である**。
送信側で本当に何も起きていないかは、条件 D(`PHASE13_TXLOG_TEST` による
送信側打刻)で送受を突き合わせて決着させる。

## 次にやること(残り)

1. **条件 A3**(アイドル 5.5 分 ×1)を採り直す。補助スクリプトの
   STOP 検出不具合は `fflush` 追加で修正済み。
2. **条件 B**(負荷: BPM ±ボタンの長押し連打と画面タッチを継続。最後に BPM を 120 へ戻し、
   戻した後の区間で判定)×2。`--from` で判定窓を切る。
3. **条件 C**(演奏中テンポ変更 120 → 180 → 120、各 1 分以上)×1。`--segments auto` で
   区間ごとの平均間隔と切替点の外れ値を見る。
4. **条件 D**(送信側打刻、`#ifdef PHASE13_TXLOG_TEST`)。`uart_write_bytes` 直前の
   `esp_timer_get_time()` をログし、σ を判定する(アイドル ≤30µs / 負荷 ≤120µs)。
   **A2 の外れ値が送信側にも現れるかをここで突き合わせる**(同時にプローブでも採る)。
   検証後にコードを削除する。
5. **SL MK3 での目視確認**(検知テンポが 120 で安定、条件 C の切替に追従)。
6. **回帰**: `./scripts/device-regress.sh --task phase13-regress`(6 本、free heap 49160 /
   largest block 31744 / WARN・ERROR 0)。clicktest が旧経路のまま動くことも確認。
7. **文書更新**: `docs/architecture.md` §10(ステップ 3 の完了日)、`docs/hostapi.md` §6
   要件 1、`docs/status.md`、`docs/lessons.md`(SD シードの教訓・ALSA キューの教訓)、
   `docs/workflow.md`(測定ツールの使い方)。
8. 検証専用コードの残存確認(`git grep PHASE13 -- src scripts wasm-apps`)。
