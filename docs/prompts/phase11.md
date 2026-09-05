# Phase 11: 新アーキテクチャの実装(移行ステップ 1〜3)

## 目的

Phase 10 で確定した新アーキテクチャ(`docs/architecture.md`、承認済み Host API 仕様
`docs/hostapi-next.md`)のうち、**移行表(`docs/architecture.md` §10)のステップ 1〜3**
を実装する。

本フェーズのゴールは **ステップ 3**: metronome を新 API で書き直し、
`midi_loopback` の E1 統計で Phase 09c の実測と前後比較し、
**「クロック欠落 61% → 0 件」を実アプリで再現する**こと。
これが本改訂の価値を最初に実証する地点である。

ステップ 4 以降(既存クリックスケジューラの置換、`hostapi_click_schedule` /
`hostapi_tone_schedule` の削除、`hostapi_midi_send` の副作用削除)は
**挙動変更を含むため本フェーズには含めない**(別フェーズ)。

## 位置づけと前提

- **設計は確定済み。** `docs/architecture.md`(§0〜§12)と `docs/hostapi-next.md`
  (全 12 関数のシグネチャ・ABI レイアウト・エラーコード・§8 のコード片)を
  実装の正本とする。設計判断の理由は `docs/architecture.md` §11 に記録されており、
  **§11-1 の「意図的に放棄した性質」は削除しないこと。**
- セッション開始時に `docs/architecture.md`、`docs/hostapi-next.md`、
  `docs/lessons.md`、`docs/workflow.md` を通読すること。
- Phase 10 の実測値(P10-1〜P10-5、`docs/results/phase10.md`)はすべて有効な前提。
  特に以下は実装上の制約である:
  - WASM アプリ実行中の internal RAM largest free block は **13〜14KB**。
    native 側の静的追加は **4KB が実証済みの安全域、8KB は未検証、12KB は破綻**。
  - L0 ディスパッチャは **LVGL・ファイルシステム・オーディオ書き込みのロックと
    一切共有しない**(§6「ロック規律」)。P10-3 の最悪 104µs はロックなしの
    試験コードでの値であり、実装で自動的に再現される値ではない。
  - 再アームは **絶対時刻グリッド基準**(前回の予定時刻 + 間隔)。発火時刻からの
    相対加算は禁止。
  - 音声再生と共存する常駐タスクは audio_player(優先度 3)より低い優先度にする。
- `wasm-apps/midi_loopback/` の E1 統計(ヒストグラム・ロバスト統計・外れ値・
  BPM 分布・STOP 時のシリアルダンプ)は恒久機能として存在する。本フェーズの
  前後比較にそのまま使う。
- 実機は現在 main ビルドで正常動作中(Phase 10 の調査コードは全て削除済み)。
  Phase 10 最終回帰の free heap 値(`docs/results/phase10.md`「最終回帰」節)を
  本フェーズの回帰基準とする。

## ゲート(必須)

1. **ステップ 0(設計の穴埋め)は報告 → 明示的承認の後にのみ次へ進む。**
   `shared/hostapi_defs.h` の変更(ステップ 2)は ABI 変更であり、既存規約どおり
   **調査報告 → 明示的承認のゲート**を通す。ステップ 0 の承認をもってこのゲートを
   兼ねる(承認前に `hostapi_defs.h` に触らない)。
2. 各ステップの完了条件(下記)を満たすまで次のステップに進まない。
   特に **ステップ 1 完了時の largest free block 確認**を飛ばさないこと。
3. ステップ 3 の前後比較で「後」に外れ値が残った場合、**設計に合わせて解釈を
   曲げず、実測を報告して停止する**(09c / 10 と同じ確証バイアス対策)。
4. 検証専用コードは `#ifdef PHASE11_*_TEST` で囲み、検証後に削除する。
   恒久コード(L0/L1、新 API、新 metronome)はガードしない。

## ステップ 0: 設計の穴埋め(実装前、承認ゲートあり)

Phase 10 のレビューで「Phase 11 前に解決する」とした設計の穴を、実装に入る前に
仕様として確定させる。**コードは書かず**、`docs/hostapi-next.md` への追記案と
根拠を報告し、承認を待つ。

### 0-1. `seq_write` の部分受理と note-on / note-off の対応

現状の仕様: `hostapi_seq_write` は受理した件数 `n` を返し、キューに空きがなければ
要求より少ない(0 もありうる)。一方 §10 の L2 実装イメージは
`if n == 0 { break; }` で、**`0 < n < 件数` のとき残りを捨てている**。
この組み合わせでは、飽和時に note-on だけが受理され対応する note-off が
失われる(鳴りっぱなし)。

決めること:

- **契約をどちらに置くか。** 候補:
  - (a) **プレフィックス受理 + アプリが残りを保持する契約**: ホストは先頭から
    `n` 件を受理し、アプリは `n` 件目以降を次回の `seq_write` で再送する義務を負う。
    ホストは単純なまま。§10 の実装イメージを修正し、契約を仕様に明記する。
  - (b) **全件受理か 0 か(アトミック)**: 空きが `buf_len/16` 件未満なら 0 を返す。
    アプリは 1 回の `seq_write` を「対で崩れない単位」(1 ステップ分、
    1 拍分など)に刻む責務を負う。
  - (c) その他、調査の結果より良い案があれば提案(ホスト側でペアを追跡する案は
    L0 に音楽的意味が漏れるため、採用するなら理由を明示すること)。
- 推奨は **(a)**(App drives の原則、L0 の単純さ、優雅な劣化の性質を保つ)。
  ただし (b) のほうが L2 の実装が単純になる面もあるので、要件 1〜5
  (`docs/hostapi-next.md` §6)を通して**どちらでも語彙が増えないこと**を確認した上で
  推奨を述べること。
- 決定を `docs/hostapi-next.md` §5 の `seq_write` 仕様と §10 の実装イメージに反映する
  (承認後)。

### 0-2. 未発火 note-off の破棄(`transport_stop` / `transport_locate` / `seq_flush_after`)

いずれもキューの未発火イベントを破棄するため、**発音済みで note-off が未発火の
ノートは鳴りっぱなしになる**。現行仕様は「All Notes Off はホストが自動送出しない
(アプリが `hostapi_midi_send` で送る)」としている。

決めること:

- v1 はこの仕様(アプリ責務)のままでよいか。本フェーズの対象アプリ(metronome、
  CLICK ポートのみ)には影響しないが、**ABI として凍結する前に判断を記録する**。
- 判断結果を `docs/architecture.md` §11 に「11-8」として追記する(承認後)。
  ホスト側で自動送出する案を採る場合は、どのポート・どのチャンネルに何を送るかを
  明示すること(DIN_OUT に CC#123 を 16ch 分か、発音中ノート追跡か、等)。

### 0-3. 報告して停止

上記 0-1 / 0-2 の追記案(diff 形式)と根拠を提示し、承認を待つ。
承認後、追記をコミットしてからステップ 1 に進む。

## ステップ 1: L0 / L1 を native に実装(既存経路と並存、まだ誰も使わない)

`docs/architecture.md` §3(Clock Authority)、§5(クロックのグリッド生成)、
§6(L0 ディスパッチャ)、§7(ポート抽象と送出規律)、§9(メモリ配置)に従う。

### 実装内容(実機、`src/components/`)

- **Clock Authority**: I2S 出力サンプルカウントをレートマスターとし、
  (sample_count, esp_timer µs) の対応を 1 モジュールに閉じ込める。
  P10-1 の `on_sent` コールバック打刻方式、P10-2 の固定比(44100)を使う。
  §3 の「レート切替・チャネル再構成時の継続規則」を実装する。
  インタフェースは固定比・逐次推定のどちらの実装も受け入れられる形にする
  (Linux ホスト用)。
- **L0 キュー**: 静的 BSS 4KB = 256 件、`l0_event_t`(16 バイト、
  `hostapi_seq_event_t` と同一レイアウト)、tick 昇順の挿入ソート配列、
  同一 tick 内は書き込み順を保つ。
- **L0 ディスパッチャ**: esp_timer ワンショット 1 本、絶対時刻グリッド基準の
  再アーム、発火偏差の系統分(16〜26µs)の前倒し補正は定数で持つ(値は
  ステップ 3 の計測で調整可)。**ロック規律(§6)を必ず守る**。
- **L1**: テンポマップ / 拍子マップ(静的確保、32 件程度)、transport 状態機械
  (STOPPED / PLAYING)、playback tick ↔ song tick の写像(ループは剰余で)、
  **MIDI クロックの 40 tick グリッド生成**(キューを消費しない)。
  `transport_start` で 0xFA、`transport_stop` で 0xFC を送出しクロックを止める
  (§11-7)、`transport_continue` で 0xFB。
- **ポート抽象**: v1 は **DIN_OUT** と **CLICK** の 2 ポートを実装する。
  DIN_OUT は §7 の送出規律(`uart_write_bytes`、一括書き込み上限 4 バイト、
  大バーストの分割、リアルタイムバイトの割り込み挿入)。
  CLICK は既存のトーンパレット(`hostapi_tone_define` のスロット)を発火時刻に
  鳴らす。**既存の `click_schedule` / `tone_schedule` 経路とどう共存させるか
  (音声側の呼び出し口をどこで共有するか)を先に調査して報告すること。**
  USB_MIDI / SYNTH は enum の予約のみ。
- **既存経路は一切触らない**: `Midi_NotifyBeatScheduled` / `Midi_NotifyBeatFired`、
  現行のクリックスケジューラ、`hostapi_midi_send` の Start/Stop 副作用はそのまま
  残す(廃止はステップ 4〜5)。本ステップの L0/L1 は誰からも呼ばれない。

### 完了条件

- 実機ビルドが通り、**既存アプリ 7 種の挙動が不変**(free heap が Phase 10
  最終回帰と一致、WARN/ERROR なし)。
- **静的 4KB(+ テンポマップ等)追加後も WASM 起動 OK**。全アプリで
  largest free block を記録し、Phase 10 最終回帰(31744)との差を報告する。
  差が想定(追加した静的量)を超える場合は停止して報告。
- L0/L1 に対する**ホスト内蔵の自己検査**(`#ifdef PHASE11_L0_SELFTEST`):
  起動時にキューへ既知パターンを積み、tick 順・安定順序・満杯時の受理数・
  `flush_after` の件数を assert する。検証後に削除。

## ステップ 2: Host API 追加(実機 + Linux ホスト同時)

ステップ 0 の承認を受けて `docs/hostapi-next.md` §8 のコード片を
`shared/hostapi_defs.h` へ取り込み、全 12 関数を実装する。

- `hostapi_transport_{start,stop,continue,locate,get_position}`
- `hostapi_tempomap_{set_tempo,set_meter,set_loop}`
- `hostapi_seq_{write,flush_after,filled_until}`
- `hostapi_time_us_to_tick`

### 要件

- **既存 API のシグネチャ・挙動は不変。** `HOSTAPI_NATIVE_SYMBOLS(X)` への追記のみ。
- **Linux ホスト(`hosts/linux/`)にも同時実装する。** Linux には I2S サンプル
  カウントがないため、Clock Authority のレートマスターは v1 では
  `CLOCK_MONOTONIC` 等の単一時計でよい(§3「環境差」の逐次推定はブラウザホスト
  Phase A の課題。本フェーズではインタフェースが差し替え可能であることのみ確認)。
  DIN_OUT は既存の ALSA シーケンサ経由、CLICK は既存の SDL 音声経路へ接続する。
  採用した方式を `docs/results/phase11.md` に記録する。
- エラーコード・境界条件(`buf_len < 32`、STOPPED 中の `filled_until`、
  PLAYING 中の `start` は -1、など)は `docs/hostapi-next.md` の記述どおり。
- `docs/hostapi-next.md` の置き場所は本ステップ完了後に整理する
  (`hostapi_defs.h` に取り込んだ後の仕様書としての位置づけを提案し、
  承認を得てから移動・改名する。勝手に削除しない)。

### 完了条件

- 実機・Linux ともにビルドが通り、**既存アプリ 7 種の回帰なし**(実機 free heap、
  Linux は `app_init=0` / `app started` / `app stopped`、警告 0)。
- 新 API を叩く**最小の検証用 WASM アプリ**(`wasm-apps/seq_smoke/` 等、
  `PHASE11_*_TEST` 相当として本フェーズ末に削除するか、`bench` のように
  恒久的に残すかを提案)で、実機・Linux 双方において:
  - `transport_start` → 0xFA と 24ppqn クロックが DIN_OUT に出る
    (実機は `midi_loopback` の E1 で受信確認、Linux は `aseqdump`)。
  - `seq_write` した DIN_OUT イベント(Note On/Off)が tick どおりの順序・
    タイミングで出る。
  - `seq_write` した CLICK イベントが鳴る。
  - `tempomap_set_tempo` を PLAYING 中に投入してもキュー積み直しなしで
    テンポが切り替わる(クロック間隔の変化を E1 で確認)。
  - `transport_stop` で 0xFC が出てクロックが止まる。
  - 同一 `.wasm` が実機・Linux で同じ挙動。

## ステップ 3: metronome を新 API で書き直す(本フェーズの本丸)

### 実装

- **既存の `wasm-apps/metronome/` は残す**(ステップ 4 の回帰対象「metronome
  (旧版)」として必要)。新版は別ディレクトリ(名前は `metronome2` 等を提案、
  承認後に確定)として作る。
- 新版は `docs/hostapi-next.md` §6「要件 1: 高精度メトロノーム」の表どおり:
  クリックは `seq_write`(port=CLICK、OP_TONE、強拍/弱拍で `param` を切替)、
  開始/停止は `transport_start` / `transport_stop`、**MIDI クロックはアプリが何も
  しない**(`hostapi_midi_send` で Start/Stop を送らない。二重送出禁止)。
  テンポ変更は `tempomap_set_tempo(次の小節頭の song_tick, upq)`。
  拍表示は `transport_get_position` の bar / beat。
- L2 の供給ループはステップ 0 で確定した契約に従う(部分受理の扱いを正しく実装)。
- UI・音量・テンポ 1 刻み等の機能は旧版と同等(Phase 7D の仕様)。
  **アプリは実時間を一切扱わない**(tick のみ)。
- 旧版と新版で**クロックが二重に出ないこと**を確認する(旧版の
  `Midi_NotifyBeatFired` 経路は新版からは起動しないはず。起動していたら報告)。

### 前後比較(E1 統計、09c と同一プロトコル)

同一セッション・同一配線で、`midi_loopback` の E1 を用いて:

- **前**: 現行 main の `metronome`(旧版)を 120bpm で起動 → E1 を 09c と
  同じ測定時間で **3 回**。
- **後**: 新版 metronome を 120bpm で起動 → E1 を同じ条件で **3 回**。
- 加えて、**最悪負荷**(mp3 再生 + タッチ操作、P10-3 と同条件)下で新版を **2 回**。
  ※ 負荷条件はユーザーの物理操作が必要。依頼のタイミングを事前に伝えること。
- **SL MK3 実機**に接続し、検知テンポが 120 で安定することをユーザーが目視確認
  (P10-5 と同様)。

判定基準(「後」に対して):

- 外れ値 **0 件**、見かけ BPM が **単峰**(09c の二峰性の消失)。
- 受信クロック数 / 期待数 = **100%**(欠落 0、追いつき 0)。
- 平均間隔 **20833µs**、σ は P10-3 の水準(アイドル ~21µs、負荷時 ≤114µs)。
  P10-3 より明確に悪い場合は、ロック規律(§6)の違反を疑って報告する。

## スコープ外

- 移行ステップ 4 / 4b / 5 / 6(既存クリックスケジューラの L0 置換、
  `click_schedule` / `tone_schedule` の削除、`hostapi_midi_send` の副作用削除、
  内蔵音源ポート)
- 旧版 metronome の削除(ステップ 4b)
- Song Position Pointer の送出(v1 スコープ外、9c から持ち越し)
- STOPPED 中のクロック送出(§11-7 で「止める」に確定済み。変更しない)
- キュー深さ 8KB への拡張(必要になった場合は再検証の上、別途)
- PSRAM の有効化、SDMMC ハングの原因調査(独立課題)
- ブラウザホスト(Phase A)、Expression pedal / ADS1115
- SMF パーサ、シーケンサー、ドラムマシン(語彙が増えないことは Phase 10 で確認済み)

## 実行環境に関する指示

**実機ビルド、Linux ホスト用ビルド、flash、monitor、カメラ撮影など、シェルで
実行するものはすべて herdr の pane を作成して実行すること。** 直接実行は行わない。
pane 構成、コマンド、タイムアウト値は `docs/workflow.md` に従い、pane 操作は
`scripts/hpane.sh` を使用する。セッション開始時に `docs/workflow.md` を通読すること。

flash 前に `esp32-monitor` の docker コンテナがシリアルポートを保持していないか
`docker ps` で確認する(既知の教訓)。

ループバック配線(OUT→IN)を挿したまま受信をドレインしないアプリを動かすと
`MIDI RX: ring buffer full` が出る(Phase 10 最終回帰で特定済みの構成依存挙動)。
回帰測定時はこの警告を Phase 11 の変更と混同しないこと。

## 完了条件

- ステップ 0 の追記(`docs/hostapi-next.md` §5 / §10、`docs/architecture.md` §11-8)
  が承認・コミットされている。
- ステップ 1〜3 の各完了条件を満たしている。
- ステップ 3 の前後比較が `docs/results/phase11.md` に記録されている
  (方法、生データの所在 `captures/phase11/`、統計表、判定)。
- 検証専用コード(`PHASE11_*_TEST`)が削除され、`git diff` で恒久コードのみが
  残っていることを確認済み。
- 既存アプリ 7 種の回帰なし(実機 free heap / largest free block、Linux 起動)、
  同一 `.wasm` が Linux ホストでもエラーなく起動する。
- `docs/status.md` の「現在地」が更新され、`docs/lessons.md` に本フェーズで得た
  教訓(あれば)が追記されている。
- `docs/architecture.md` §10 の移行表に、ステップ 1〜3 の完了日と結果へのポインタが
  追記されている。

## 報告フォーマット

1. ステップ 0: 追記案(diff)と根拠、要件 1〜5 との突き合わせ結果 → **ここで停止**
2. ステップ 1: 追加した静的メモリ量、全アプリの largest free block 表(Phase 10 比)、
   自己検査の結果、CLICK ポートと既存音声経路の共存方法
3. ステップ 2: 実機 / Linux の検証結果表(12 関数 × 確認項目)、Linux の
   Clock Authority 方式、`hostapi-next.md` の置き場所の提案
4. ステップ 3: 前後比較の統計表(前 3 回 / 後 3 回 / 負荷 2 回)、09c・P10-3 との
   対照、SL MK3 の目視結果、判定
5. 発見した設計との乖離・新たに判明した制約(あれば)、およびステップ 4 以降への
   申し送り
