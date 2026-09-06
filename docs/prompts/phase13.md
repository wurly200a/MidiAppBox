# Phase 13: metronome を新 API で書き直す(移行ステップ 3、絶対値目標で判定)

## 目的

`wasm-apps/metronome/` を**新アーキテクチャの Host API(`transport_*` / `tempomap_*` /
`seq_*`)だけで書き直し**、MIDI Clock 出力が**絶対値目標**(下記)を満たすことを
実機で示す。旧版との前後比較は行わない(Phase 09 の実装は実用に耐えないことが
09c で確定しており、「前」の再測定に価値がない)。

これは `docs/architecture.md` §10 の**移行ステップ 3**であり、本改訂の価値
(「09c の欠落 61% → 0 件」を実アプリで)を最初に実証する地点である。

## 位置づけと前提

- セッション開始時に `docs/workflow.md`、`docs/lessons.md`、`docs/architecture.md`
  (§5〜§7、§10〜§11)、`docs/hostapi.md`(特に §5 `seq_write` の**プレフィックス受理
  契約**、§6 要件 1、§10 の L2 実装イメージ)、`docs/results/phase11.md`
  (「Phase 12 への申し送り」)、`docs/results/phase12.md`(「Phase 13 への申し送り」)を
  通読すること。
- **既存の `wasm-apps/metronome/` を上書きで書き直す**(別ディレクトリに分けない)。
  旧版の機能(Phase 7B/7D: BPM 40〜240、±1/±5 と長押し連打加速、拍子 2/3/4/6、
  START/STOP、拍ランプ、アクセント音スロット、V−/V+ 音量)は**すべて維持**する。
  旧版のロジックは git 履歴に残る(削除前にタグ `pre-metronome-rewrite` を打つ)。
- **旧経路はまだ生きている**(`hostapi_click_schedule` / `hostapi_tone_schedule`、
  `hostapi_midi_send` の Start/Stop 副作用、`Midi_NotifyBeat*`)。廃止は移行
  ステップ 4〜5(別フェーズ)。新 metronome はこれらを**一切呼ばない**。特に
  `hostapi_midi_send` で Start/Stop を送ると**クロックが二重に出る**。
- 回帰は `./scripts/device-regress.sh --task <名前>` を既定とする(Phase 12、
  物理操作なし約 100 秒、対象 6 本)。基準値は `docs/results/phase12.md` の最終回帰
  (free heap 49160、largest block 31744)。
- **SD のマウント経路に注意**: SDSPI フォールバックに落ちると largest free block が
  15,360 になり WASM が全滅する(Phase 12 の発見)。ファームは同じなのに全部落ちたら
  まずログの `Trying SDMMC` / `falling back to SDSPI` を確認し、フォールバックして
  いれば**ユーザーにボードの電源入れ直し(USB 抜き差し)を依頼する**。ソフトリセットでは
  直らない。
- 測定は **実機 MIDI OUT → UM-ONE → Linux PC** で採る(実機は WASM アプリを 1 つしか
  動かせないため、`midi_loopback` との同時実行は不可)。**Linux 側で ALSA が使える
  環境はユーザーが用意する**(申し送り 2)。セッション冒頭で `aconnect -l` 等で
  UM-ONE が見えることを確認してから進む。
- P10-5 の受信打刻バッチング(±1.2ms)は**ノートとクロックが混在するとき**に出る。
  本フェーズの測定条件(クロックのみ、ノートなし)では発生しない。UM-ONE 経由の
  受信は USB の 1ms フレームを通るので、**受信側の σ は 1ms 弱にぼやける**。
  σ は受信側では判定しない(下記)。

## ゲート(必須)

1. **設計メモ(下記ステップ 1)は報告 → 承認の後に実装に入る。**
2. 測定ツール(ステップ 2)は実装前に方式を提案し、承認後に作る。`scripts/` と
   `docs/workflow.md` の変更は §5 の規約どおり。
3. **絶対値目標を満たさない場合、設計に合わせて解釈を曲げず、実測を報告して停止する**
   (09c / 10 / 12 と同じ確証バイアス対策)。特に外れ値が残る場合は、
   ロック規律(§6)の違反と `seq_write` 契約の実装ミスを最初に疑うこと。
4. 検証専用コードは `#ifdef PHASE13_*_TEST` で囲み、検証後に削除する。
   恒久コード(新 metronome、測定ツール)はガードしない。
5. Host API / ABI は変更しない。`docs/hostapi.md` の仕様どおりに書けない箇所が
   出たら、回避せず報告して停止する(それ自体が Phase 10 の設計検証の結果になる)。

## ステップ 1: 設計メモ(実装前、承認ゲート)

新 metronome の構造を `docs/hostapi.md` §6「要件 1」の表に沿って短く書く。
最低限、以下を決めて報告する:

- **テンポの表現**: `tempomap_set_tempo(at_song_tick, upq)`、`upq = 60_000_000 / bpm`
  (整数除算の端数の扱いを明記。120bpm は割り切れるが 132bpm 等は割り切れない。
  端数は L1 側の tick→µs 変換で吸収されるのでアプリは丸めてよい、で足りるはず)。
- **演奏中のテンポ変更**: 次の小節頭の song_tick に `set_tempo` を投入する方式
  (要件 2 と同じ)。長押し連打で 1 小節内に複数回変更が来る場合の扱い
  (最後の値だけ有効にする、同じ at_tick への再設定が上書きになるかは
  `docs/hostapi.md` §4 の仕様を確認)。
- **拍子変更**: 演奏中は次の小節頭で `tempomap_set_meter`、または STOPPED 中のみ
  許可。どちらにするか提案(旧版の挙動を確認して合わせる)。
- **クリックの供給(L2)**: `seq_write` に port=CLICK / `HOSTAPI_SEQ_OP_TONE`、
  1 拍目は `param=アクセント用スロット`、他は既定スロット。horizon は 2 小節程度。
  **プレフィックス受理契約に従い、未受理分を保持して次回再送する**(§10 の実装
  イメージどおり)。`transport_stop` / `locate` 後は保持分を破棄する。
  メトロノームは無限に続くのでループ(`set_loop`)は使わない。
- **拍ランプ**: `transport_get_position()` の bar / beat から描画。`app_tick`
  100ms 周期のため最大 100ms の表示遅れがあることを許容する(音のタイミングには
  無関係。旧版と同じ制約)。
- **START/STOP**: `transport_start` / `transport_stop`。stop でキューは破棄される。
  CLICK トーンは固定長ワンショットなので鳴りっぱなしは発生しない(§11-8 の
  影響なし)。**`hostapi_midi_send` は呼ばない**。
- **音量**: `hostapi_audio_set_volume`(既存 API、変更なし)。
- **旧経路の不使用の確認方法**: 実機ログで `Midi_NotifyBeat*` 経路が一度も通らない
  ことをどう確認するか(既存ログの有無、なければ `PHASE13_*_TEST` で一時ログ)。

## ステップ 2: 測定ツール(Linux 側)

`midi_loopback` の E1 統計と同等の集計を **Linux PC 側**で行うツールを作る
(置き場所は `scripts/` か `tools/` を提案)。要件:

- UM-ONE からの受信を ALSA シーケンサ経由で読み、**受信時刻は PC の単調時計**
  (`CLOCK_MONOTONIC` 相当、µs)で打刻する。ALSA シーケンサのタイムスタンプ付き
  キューを使うか、受信スレッドで打刻するかは提案(既存の `hosts/linux/hostapi_midi.c`
  の実装を参考にしてよい)。
- 集計項目(E1 と揃える): 0xF8 総数、期待数(0xFA〜0xFC 間の経過時間 × 公称
  レート)、clocks/expected、クロック間隔の min / mean / max、ヒストグラム、
  **外れ値**(公称の 1.5 倍以上、または 0.5 倍以下)の件数と発生時刻、
  直近 24 クロック移動平均からの**見かけ BPM の分布**(単峰 / 二峰の判定)。
  0xFA / 0xFB / 0xFC の件数。
- 出力は Markdown 表 + 生データ CSV(`captures/phase13/`)。
- 使い方を `docs/workflow.md` に追記する(§3 に「MIDI Clock 測定」として。承認後)。

**ツール自体の妥当性確認**: 実装後、まず **seq_smoke**(Phase 11 で Linux 送出
σ33µs、実機で欠落なしを確認済み)を実機で走らせて UM-ONE 経由で採り、
Phase 11 の結果(0xFA/0xFC 1/1、クロック数が期待どおり)と整合することを確認する。
これで「ツールが欠落を作っていない」ことを先に担保する。

## ステップ 3: 実装

- ステップ 1 の設計メモどおりに `wasm-apps/metronome/src/lib.rs` を書き直す。
  `extern` 宣言から `hostapi_click_schedule` / `hostapi_tone_schedule` /
  `hostapi_midi_send` を**削除**する(呼ばないことをリンク時に保証する)。
- 同一 `.wasm` が Linux ホストでも動くこと(`hosts/linux/` の DIN_OUT は ALSA 経由、
  CLICK は SDL 音声経路。Phase 11 で接続済み)。
- ビルド後、実機で手動確認(§3.3、カメラ): START/STOP、BPM ±1/±5/長押し、拍子変更、
  アクセント音、音量、拍ランプ。**ここはユーザーの物理操作が要る。**

## ステップ 4: 測定と判定

### 条件

同一セッション・同一配線で、すべて **120bpm・4/4**、測定時間は 09c の E1 と同じ
(`docs/results/phase09c.md` を確認。最低 5 分):

| # | 条件 | 回数 | 備考 |
|---|---|---|---|
| A | アイドル(START 後、無操作) | 3 | 基本条件 |
| B | 負荷(START 後、BPM ±ボタンの長押し連打と画面タッチを継続。**ただし BPM は最後に 120 へ戻す**) | 2 | ユーザーの物理操作。テンポ変更区間は判定から除外し、120 に戻した後の区間で判定 |
| C | 演奏中テンポ変更(120 → 180 → 120、各 1 分以上) | 1 | 切替が小節頭で起き、各区間の間隔が公称に一致すること。切替時に外れ値が出ないこと |
| D | 送信側打刻(`#ifdef PHASE13_TXLOG_TEST`、`uart_write_bytes` 直前の `esp_timer_get_time()`) | 1 | **σ の判定はここで行う**(受信側は USB でぼやける)。検証後に削除 |

### 絶対値目標(A / B の全回で満たすこと)

| 項目 | 目標 | 根拠 |
|---|---|---|
| 外れ値(公称の 1.5 倍以上 / 0.5 倍以下) | **0 件** | P10-3: 30,268 発で 0 |
| clocks / expected | **100%**(境界の ±1 発は許容) | 同上 |
| 見かけ BPM の分布 | **単峰**(115〜119 の第 2 峰がない) | 09c の二峰性の消失 |
| クロック間隔の平均 | **20833µs ± 10µs** | P10-2: fs 偏差 −0.00ppm |
| 送信側 σ(条件 D) | **アイドル ≤ 30µs、負荷 ≤ 120µs** | P10-3: 21µs / ≤114µs |
| 0xFA / 0xFC | 1 / 1(START/STOP 各 1 回のとき) | 二重送出がないこと |

送信側 σ が P10-3 より明確に悪い場合は、**ロック規律(§6)の違反**を第一の疑いとして
報告する(P10-3 の値はロックなしの試験コードの値であり、実装で自動的に出る値ではない)。

### 外部機器での確認

SL MK3 を実機 MIDI OUT に接続し、検知テンポが **120 で安定**することをユーザーが目視
確認(P10-5 と同様)。条件 C の切替で 180 → 120 に追従することも確認。

### Linux ホスト

同一 `.wasm` を Linux で起動し、`app_init=0` / `app started` / `app stopped`、
警告 0 を確認。Linux 側の送出精度は Phase 11 で確認済み(σ33µs)なので測定は不要。

## ステップ 5: 回帰と文書

- `./scripts/device-regress.sh` で 6 本の回帰合格(free heap / largest block が基準値、
  WARN/ERROR 0)。新 metronome の `.wasm` サイズ差を記録する。
- **clicktest が旧経路のまま動くこと**(旧経路の回帰。移行ステップ 4 の対象として残す)。
- `docs/architecture.md` §10 の移行表に、ステップ 3 の完了日と
  `docs/results/phase13.md` へのポインタを追記。
- `docs/hostapi.md` §6「要件 1」に「実装済み(Phase 13)、追加語彙なし」を追記。
  書けなかった箇所があれば(ゲート 5)、それを記録する。
- `docs/status.md`、`docs/lessons.md`(新たな教訓)、`docs/workflow.md`(測定ツール)。
- 測定結果の表・ヒストグラム・生データの所在は、後日の Zenn 記事の素材になるので
  `docs/results/phase13.md` に**そのまま引用できる形**で残す(09c の「61%」と
  本フェーズの「0 件」を並べられる表を 1 つ用意する)。

## スコープ外

- 移行ステップ 4 / 4b / 5(旧クリック経路の置換・削除、`hostapi_midi_send` の
  副作用削除)— 次フェーズ候補
- PSRAM の本番反映(Phase 14 候補: ヒープ領域の確認 → WAMR プール移動 →
  app_tick 計測 → SDMMC 共存の再調査 → E1 タイミング検証)
- Song Position Pointer、STOPPED 中のクロック送出(§11-7 で確定済み)
- 楽曲メトロノーム(セクション構成・小節毎 PC)— 要件 2、別フェーズ
- `midi_loopback` の送信側を新 API に切り替えること(ステップ 4 で扱う)
- Host API / ABI の変更

## 実行環境に関する指示

**実機ビルド、Linux ホスト用ビルド、flash、monitor、カメラ撮影、測定ツールの実行など、
シェルで実行するものはすべて herdr の pane を作成して実行すること。** 直接実行は
行わない。pane 構成、コマンド、タイムアウト値は `docs/workflow.md` に従い、pane 操作は
`scripts/hpane.sh` を使用する。セッション開始時に `docs/workflow.md` を通読すること。

flash 前に `esp32-monitor` の docker コンテナがシリアルポートを保持していないか
`docker ps` で確認する(既知の教訓)。`device-regress.sh` 終了後のコンテナ残存も同様。

条件 B・C と SL MK3 確認、ステップ 3 の手動確認はユーザーの物理操作が要る。
**依頼のタイミングを事前に伝え、完了の返答を待ってから次へ進む。** セッションを
分けるなら、ステップ 1(承認)→ ステップ 2〜3(ツールと実装、seq_smoke による
ツール確認までは無人)→ ステップ 4〜5、の切れ目が自然。

## 完了条件

- ステップ 1 の設計メモが承認されている。
- 測定ツールが seq_smoke で妥当性確認済みで、使い方が `docs/workflow.md` にある。
- 新 metronome が旧版の全機能を維持し、旧経路の API を `extern` から外した状態で
  実機・Linux 双方で動作する。
- 条件 A〜D の測定結果が `docs/results/phase13.md` に記録され、**絶対値目標を
  すべて満たしている**(満たさない場合はそこで停止・報告されている)。
- SL MK3 での目視確認が記録されている。
- 検証専用コード(`PHASE13_*_TEST`)が削除され、`git grep PHASE13 -- src scripts wasm-apps`
  で残存なし。
- 6 本の自動回帰に合格し、clicktest(旧経路)も動作している。
- `docs/architecture.md` §10、`docs/hostapi.md` §6、`docs/status.md`、
  `docs/lessons.md`、`docs/workflow.md` が更新されている。
- `git status --porcelain` がクリーン、コミットはステップ単位(英語メッセージ)。

## 報告フォーマット

1. ステップ 1: 設計メモ(テンポ表現、テンポ/拍子変更、L2 供給、ランプ、旧経路
   不使用の確認方法)→ **ここで停止**
2. ステップ 2: 測定ツールの方式提案 → **停止** → 実装後、seq_smoke による妥当性確認の表
3. ステップ 3: 実装の要点、`.wasm` サイズ、Linux 動作、手動確認の依頼手順
4. ステップ 4: 条件 A〜D の統計表(目標との対照)、ヒストグラム、見かけ BPM 分布、
   SL MK3 の目視結果、判定。**09c の「61%」と並べた 1 表**
5. ステップ 5: 回帰表、文書更新の一覧
6. ステップ 4 以降(旧経路の置換・削除)への申し送り: 新 metronome で不要になった
   ホスト側コード、`midi_loopback` の送信側切替の見通し、Phase 10 の設計で
   実装時に困った点(あれば)
