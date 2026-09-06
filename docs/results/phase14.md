# Phase 14 実施記録 — 旧経路の削除(移行ステップ 4b / 5)

対応する指示書: `docs/prompts/phase14.md`
関連: `docs/architecture.md`(§7 / §10 / §11-3 / §11-8)、`docs/hostapi.md`(§7)、
`docs/results/phase13.md`(次フェーズへの申し送り)。

## セッション冒頭の環境確認(2026-09-06)

| 項目 | 結果 |
|---|---|
| `hpane.sh ensure unix-build` | pane `wC:pQ` を確認、`echo hello` exit 0 |
| `docker ps`(flash 前) | 常駐監視コンテナなし |

## ステップ 1: midi_loopback を新 API へ移す

### 変更点

`wasm-apps/midi_loopback/src/lib.rs` を編集(上書きではなく差分修正。Stage 1〜3 の
受信ドレイン・16進表示・BPM移動平均・E1 統計は変更なし)。

- `extern` から `hostapi_click_schedule` / `hostapi_midi_send` を削除し、
  `hostapi_transport_start` / `hostapi_transport_stop` /
  `hostapi_tempomap_set_tempo` / `hostapi_tempomap_set_meter` を追加。
- `app_init()` で `tempomap_set_meter(0,4,4)` / `tempomap_set_tempo(0,500000)`
  (120bpm 固定)を 1 回設定(STOPPED 中の初期設定、`docs/hostapi.md` §4)。
- START/STOP は `hostapi_transport_start()` / `hostapi_transport_stop()` に置換。
  旧版の「毎 tick `hostapi_click_schedule` 再予約」ループは削除(クロックは
  L1 がテンポマップ + transport からグリッド生成するため、アプリは何もしない)。
- **可聴クリックは供給しない**と判断した。理由: 本アプリの目的は受信統計
  (クロックの欠落・ジッタ測定)であり、`seq_write` での追加供給は測定条件を
  複雑にするだけで診断上のメリットがない。E1(ヒストグラム・ロバスト統計・
  外れ値・見かけ BPM 分布)は既存のまま恒久機能として維持。
- 未使用になった `hostapi_now_ms` の extern 宣言も削除(旧クリックループの
  `now` 計算にのみ使っていた)。

### 静的(ビルド後の import 確認)

```
strings midi_loopback.wasm | grep hostapi_
  hostapi_draw_text / hostapi_fill_rect / hostapi_midi_recv / hostapi_poll_event /
  hostapi_tempomap_set_meter / hostapi_tempomap_set_tempo /
  hostapi_transport_start / hostapi_transport_stop
```

旧経路の 2 関数(`hostapi_click_schedule` / `hostapi_midi_send`)は import に
含まれない。`.wasm` サイズ: 5,787 → 5,764 B(-23 B。旧クロック導出ロジックの
削除分)。

### Linux ホスト確認

`./build/midibox_host ../../wasm-apps/midi_loopback/midi_loopback.wasm` を単発実行。
`app_init=0` → `app started` → (ESC) → `app stopped`、警告 0、残留プロセスなし。

### 実機の受け入れ測定(実測、絶対値目標)

測定条件: 実機 MIDI OUT → **自機 MIDI IN** のループバック配線(ユーザーの物理作業)、
120bpm・4/4、midi_loopback アプリで START → 約 6.3 分保持 → STOP。

E1 の詳細統計(ヒストグラム・ロバスト平均/σ・外れ値件数・BPM 分布)は画面外
センチネル座標(`LOG_X=9000`)へ描画されるため、Phase 9c と同じ手法で
`src/components/wasm_runtime/hostapi.cpp` の `native_hostapi_draw_text` に
検証専用ガード `#ifdef PHASE14_STATLOG_TEST`(x>=1000 の描画を `ESP_LOGI` へ
転送)を一時追加して実機シリアルログへ転送した。測定後にガードごと削除し、
`git diff` で `hostapi.cpp` が変更前と一致することを確認済み
(`git grep PHASE14 -- src scripts wasm-apps tools` が空)。

あわせて `dump_stop_stats()` に「clocks / expected」の行(画面表示と同じ
算出式)を追加し、カメラ静止画に頼らず確認できるようにした(この行は
恒久機能として `lib.rs` に残す。off-screen 描画のみで実害なし)。

初回測定(約4.3分、途中で補助行の追加が必要と判明したため再測定):

```
H 0 0 11922 529 0 0 0 0
R med~=20750 exN=12451 exMean=20838
R exSig=81 exMin=20620 exMax=21247 outN=0
O cnt=0
B 0 0 0 12428 0
```

外れ値 0 件・単峰は確認できたが、5 分以上という測定条件を満たしているか
不確実だったため(ログからの推定で約 4.3 分)、`dump_stop_stats()` に
clocks/expected の行を追加して再測定した。

**確定測定**(約 6.3 分、`captures/phase14/monitor2.log`):

```
H 0 0 17468 786 0 0 0 0
R med~=20750 exN=18254 exMean=20836
R exSig=82 exMin=20616 exMax=21231 outN=0
O cnt=0
C clocks=18255 exp=18255
B 0 0 0 18231 0
```

| 項目 | 目標 | 実測 |
|---|---|---|
| 外れ値(公称の1.5倍以上/0.5倍以下) | 0件 | **0件** |
| clocks / expected | 100%(境界の±1発は許容) | **18255 / 18255 = 100.00%** |
| 見かけ BPM の分布 | 単峰 | **単峰**(24クロック移動平均の判定対象 18231 件が全て同一バケット) |
| クロック間隔の平均 | 20833µs ± 10µs | **20836µs**(公称 20833.33 との差 +2.7µs) |

**絶対値目標を全項目クリア。** `MIDI RX: ring buffer full` を含む WARN/ERROR は
ログに 0 件(受信ドレインが機能している証拠)。

### 検証専用コードの後始末

- `hostapi.cpp` の `PHASE14_STATLOG_TEST` ガード(`#define` と `#ifdef` ブロック)を
  削除し、`git diff` で変更前と完全一致を確認。
- クリーンな(ガードなし)ファームウェアを再ビルド・再フラッシュした上で
  6 本の自動回帰(`phase14-step1-final-regress`)を実施(下記)。

### 回帰(削除前、6 本のまま)

`./scripts/device-regress.sh --task phase14-step1-final-regress`

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block | 判定 |
|---|---|---|---|---|---|
| touch_demo | 49136 | 49136 | +0 | 31744 | PASS |
| mp3player | 49136 | 49136 | +0 | 31744 | PASS |
| clicktest | 49136 | 49136 | +0 | 31744 | PASS |
| metronome | 49136 | 49136 | +0 | 31744 | PASS |
| midi_loopback | 49136 | 49136 | +0 | 31744 | PASS |
| seq_smoke | 49136 | 49136 | +0 | 31744 | PASS |

許容外の WARN/ERROR 0 件、**結果 PASS**。free heap は Phase 13 の基準値
(49136)と完全一致(リークなし)。

## ステップ 2: clicktest を削除する

削除前のタグ: **`pre-old-api-removal`**。

### 実施内容

| 対象 | 変更 |
|---|---|
| `wasm-apps/clicktest/` | `git rm -r` でディレクトリごと削除(承認済み。Bash 分類器が `rm -rf` / `git rm -r` を破壊的操作としてブロックしたため、ユーザーに確認のうえ実施) |
| `src/components/wasm_runtime/CMakeLists.txt` | `EMBED_FILES` から `clicktest.wasm` を削除 |
| `src/components/wasm_runtime/launcher.cpp` | `clicktest_wasm_start/end` の extern 宣言と `seed_file` 呼び出しを削除 |
| `scripts/device-regress.conf` | `APPS` から `clicktest` を削除(6 本 → **5 本**) |
| `wasm-apps/README.md` | アプリ一覧から `clicktest/` の行を削除、`midi_loopback/` の説明をステップ1の内容に更新 |
| `CLAUDE.md` | 回帰対象アプリの列挙を 5 本に更新 |
| `docs/results/phase12.md` | カバレッジ表(2026-09-06 朝時点のスナップショット)は歴史的決定根拠としてそのまま残し、末尾に Phase 14 の追記節を追加(下記) |

### カバレッジの穴が空かないことの確認

Phase 12 のカバレッジ表(`docs/results/phase12.md`)を Phase 14 時点の実装と
突き合わせて確認した。

- `click_schedule` / `tone_schedule`: clicktest 削除時点で利用者ゼロ
  (metronome は Phase 13、midi_loopback は Phase 14 ステップ1で移行済み)。
  ステップ3で API 自体を削除するため、行ごと消えるのが正しい(コード上の穴ではない)。
- `play_click`: touch_demo が唯一の残存呼び出し元になるが、これは clicktest 削除前から
  変わらず健在(touch_demo は継続して残す判断は Phase 12 で確定済み)。**穴なし。**
- `now_ms`: metronome(長押し連打の時間計測)が引き続きカバー。**穴なし。**
- `midi_send`(生バイト送出): seq_smoke が CC#119/#120 の送出で引き続きカバー
  (metronome・midi_loopback は Phase 13/14 で `midi_send` を使わなくなったが、
  API 自体は残る前提なので seq_smoke のカバレッジで足りる)。**穴なし。**
- `transport_*` / `tempomap_*` / `seq_*` / `time_us_to_tick`: 従来 seq_smoke のみだったが、
  metronome(Phase 13)・midi_loopback(Phase 14)も新たにカバーするようになった。
  **穴が空くどころかカバレッジが増えた。**

`tone_play` は元々どのアプリからも呼ばれておらず(Phase 12 の時点で判明済み)、
clicktest 削除の影響を受けない。

### バイナリサイズ

| | サイズ |
|---|---|
| 削除前(ステップ1 最終、STATLOG フックなし) | 0x100190 = 1,049,488 B |
| 削除後 | **0xffc40 = 1,048,128 B** |

**−1,360 B**(`.wasm` 1 本 + launcher の extern/seed 呼び出し分)。

### 回帰(5 本、`phase14-step2-regress`)

| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block | 判定 |
|---|---|---|---|---|---|
| touch_demo | 49136 | 49136 | +0 | 31744 | PASS |
| mp3player | 49136 | 49136 | +0 | 31744 | PASS |
| metronome | 49136 | 49136 | +0 | 31744 | PASS |
| midi_loopback | 49136 | 49136 | +0 | 31744 | PASS |
| seq_smoke | 49136 | 49136 | +0 | 31744 | PASS |

許容外の WARN/ERROR 0 件、**結果 PASS**。free heap・largest block とも
ステップ1と同一(clicktest はネイティブ側の常駐状態を持たなかったため、
フラッシュのみ減って RAM の水準は変化しない)。
