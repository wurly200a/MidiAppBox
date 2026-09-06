# Phase 12: 基盤整備(パーティション拡張・アプリ整理・実機テスト自動化・PSRAM 可否)

## 目的

Phase 13(metronome の新 API による書き直しと絶対値目標での検証)に入る前に、
開発を詰まらせる要因を先に取り除く。本フェーズは **4 つの独立した作業**からなり、
順に実施する。各作業の完了時に既存アプリの回帰を取り、次へ進む。

1. **パーティション拡張**: ファームウェア領域の残りが約 5KB しかなく、新アプリを
   1 本足すだけで溢れる。
2. **アプリ整理**: 回帰対象 10 本のうち、Host API の検証に寄与しないものを外す。
3. **実機テストの自動化**: 起動がタッチ、停止が電源キーという人手前提の経路しか
   なく、回帰のたびにユーザーの物理操作が要る。
4. **PSRAM の使用可否**: P10-4 で「有効化すると SDMMC プローブでリブートループ」
   となったまま原因未調査。WASM linear memory の逼迫(largest free block 13〜14KB)
   を根本から緩和できる可能性があるため、go/no-go を判定する。

**metronome の書き直し(旧 Phase 11 ステップ 3)は本フェーズに含めない。**
新アーキテクチャの L0/L1・Host API 12 関数(Phase 11 完了)には手を入れない。

## 位置づけと前提

- セッション開始時に `docs/workflow.md`、`docs/lessons.md`、`docs/architecture.md`
  §9・§11-2、`docs/results/phase10.md` の P10-4 節、`docs/results/phase11.md` を
  通読すること。
- 回帰の基準値は Phase 11 ステップ 2 の実機回帰(free heap 54052/54008、
  largest free block 31744、mp3player の −44B は既知)。
- 現状の事実(Claude Code は着手前に自分で再確認すること):
  - `src/components/wasm_runtime/CMakeLists.txt` の `EMBED_FILES` で `.wasm` 10 本を
    ファームウェアに埋め込み、`launcher_prepare_sd()` が起動時に `/sdcard/apps/` へ
    seed している。**ファームウェア領域の逼迫はこの埋め込みが主因**。
  - `src/sdkconfig.defaults` にフラッシュサイズ・パーティション表の指定がない
    (既定の 2MB / factory 1MB 相当で動いている)。実フラッシュは 16MB。
  - アプリ起動は LVGL メニューのタッチ、停止は電源キー短押し
    (`pwr.set_on_short_press` → `app_request_stop()`)。
  - heap の計測ログは `wasm_runtime.cpp` の `app: stopped (...), free heap %u
    (at start %u), largest block %u` に集約されている。
  - PSRAM: 8MB octal 搭載。P10-4 で有効化時に SD 初期化の SDMMC プローブで
    TG1WDT リブートループ。PSRAM 側のレイテンシベンチは未取得。

## ゲート(必須)

1. **作業 1〜4 は順に実施し、各作業の完了条件(回帰合格)を満たしてから次へ進む。**
   作業 3・4 は事前調査報告 → 承認の後に実装に入る(下記)。
2. **`docs/workflow.md` の手順・`scripts/` の変更は §5 の規約どおり提案 → 承認**
   (作業 3 が該当)。
3. **PSRAM(作業 4)は go/no-go 判定までが本フェーズ**。go の場合も、本番構成への
   反映(WAMR プールの PSRAM 移動等)は別フェーズとし、本フェーズでは調査コードを
   削除して main を no-PSRAM の状態に戻す。
4. 検証専用コードは `#ifdef PHASE12_*_TEST` で囲み、検証後に削除する。
   恒久コード(パーティション表、シリアルコマンド、回帰スクリプト)はガードしない。
5. 想定外の事象(特にフラッシュ後に起動しない、SD が見えない)は、その場で
   試行錯誤せず報告して停止する。

## 作業 1: パーティション拡張

- `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` と `partitions.csv`(`src/` 直下、
  `CONFIG_PARTITION_TABLE_CUSTOM`)を追加する。**OTA は不要**(無線スタックなし)。
  factory 1 本を主体とし、将来のためにアプリ用データ領域(`.wasm` や設定の
  置き場候補)を少量切っておくかは提案に含める(切らなくてもよい)。
- **既存の NVS・その他パーティションのオフセットが変わる場合**はその影響
  (設定の消失等)を報告に明記する。現状 NVS に永続化しているものがあるかを先に確認。
- 変更後の残容量(`idf.py size` 相当)を記録する。

完了条件: 実機ビルド・フラッシュ・起動 OK、既存アプリ全本の回帰合格
(free heap / largest block が基準値と一致、WARN/ERROR なし)、Linux ホスト影響なし。

## 作業 2: アプリ整理

### 調査(削除前に報告)

10 本(hello / demo / bars / bench / touch_demo / mp3player / clicktest /
metronome / midi_loopback / seq_smoke)について、**アプリ × Host API 関数の
カバレッジ表**を作る(各アプリがどの Host API を呼んでいるか、`.rs` の呼び出しから
機械的に抽出)。残す基準は:

> **そのアプリが検証している Host API の部分集合を、他のどのアプリも覆っていない
> こと**、または製品・計測器として固有の役割があること。

想定(調査で覆ってよい): 残す = metronome(製品)、midi_loopback(計測器)、
mp3player(SD + オーディオ経路)、seq_smoke(新 API の恒久スモーク)、
demo / touch_demo のどちらか(表示・タッチ)。候補外 = hello / bars / bench /
clicktest。clicktest は旧 `click_schedule` 経路の専用テストなので、その API を削除する
時点(移行ステップ 4b)で役目が終わるが、**それまでは残す**(旧経路の回帰対象)。
判断は表を見て提案し、承認後に実施する。

### 実施

- 削除前に **git タグ**を打つ(例: `pre-app-prune`)。削除は `wasm-apps/<name>/`
  の除去、`EMBED_FILES`、`launcher_prepare_sd()` の seed リスト、Linux ホストの
  ビルド対象、`docs/workflow.md` §3 の回帰対象リスト、`CLAUDE.md` の回帰対象記述、
  `wasm-apps/README.md` を一貫して更新する。
- **削除したアプリの seed が SD 上に残る**点に注意(`/sdcard/apps/` の既存ファイルは
  seed で上書きされるだけで消えない)。ランチャーに古い `.wasm` が残ることを許容するか、
  seed 時に管理外ファイルを掃除するかを提案する(既定は「掃除しない」= ユーザーが
  SD に置いたアプリを消さない)。

完了条件: 残したアプリ全本の回帰合格、`grep` で削除アプリ名の参照が
docs/results/(履歴)以外に残っていないこと。

## 作業 3: 実機テストの自動化

### 事前調査(実装前に報告 → 承認)

以下を調べて、方式を提案する:

1. **シリアル入力の経路**: 現行の `idf.py monitor`(USB-Serial/JTAG コンソール)
   経由で、`scripts/hpane.sh send esp32-monitor "<text>"` が実機の stdin に届くか
   (`idf.py monitor` はキー入力を転送するが、herdr 経由の行送信で改行まで届くかを
   実測する)。届かない場合の代替(`esp_console` の REPL、または monitor を使わず
   `pyserial` 直叩き)を比較する。
2. **必要なコマンドの最小集合**: `run <app>`(メニューを経由せず `/sdcard/apps/<app>.wasm`
   を起動)、`stop`(`app_request_stop()` と同じ)、`heap`(現在の free / largest を
   出力)、`ls`(apps 一覧)。**タッチ・電源キーの既存経路は残す**(製品としての
   操作系は変えない)。
3. **アプリ内操作の扱い**: metronome の START/STOP や midi_loopback の統計ダンプは
   アプリ内 UI で、シリアルからは触れない。v1 は「起動 → 一定時間 → 停止」の
   回帰(heap・警告)を自動化の範囲とし、アプリ内操作の自動化(Host API 経由の
   `hostapi_poll_event` へのイベント注入)は**別課題として設計案だけ添える**。
4. **ログの機械判定**: `captures/<task>/monitor.log` から「app started / stopped、
   free heap 開始・終了、largest block、WARN/ERROR 件数(既知の許容パターンを
   除外)」を抽出して表にする方式。既知の許容パターン(`spi_flash: Detected size`、
   mp3player の −44B)は設定ファイル化する。

### 実装(承認後)

- ファームウェア側: Kconfig(`CONFIG_MIDIBOX_SERIAL_CMD`、既定 y)で有効化される
  シリアルコマンド。**コンソールタスクの優先度は audio_player(3)より低く**、
  LVGL ロックの取り方は既存ランチャーの流儀に従う。L0 ディスパッチャとロックを
  共有しない。
- スクリプト側: `scripts/device-regress.sh`(仮名)。`docs/workflow.md` §3.2 の
  docker run 方式に乗り、モニタペインを使い、残したアプリを順に
  `run → waitfor "app started" → sleep N → stop → waitfor "app: stopped"` し、
  表(Markdown)と合否を出力する。**人間操作・カメラ(§3.3)は残す**
  (音・画面の確認は自動化しない)。
- `docs/workflow.md` §3 に「自動回帰」を追記し、**以後のフェーズの回帰はこの
  スクリプトを既定とする**旨を §2 に反映(承認後)。

完了条件: スクリプト 1 回の実行で残したアプリ全本の回帰表が出て、手動回帰と
同じ値(heap / largest / 警告)が得られること。ユーザーの物理操作なしで完走すること。

## 作業 4: PSRAM の使用可否(go/no-go 判定)

### 目的の明確化

PSRAM の用途候補は **WAMR のメモリプール / WASM linear memory / LVGL 描画バッファ**
であり、**L0 キュー・テンポマップは対象外**(internal 固定。§9 の方針は変えない)。
判定はこの用途に対して行う。

### 調査(仮説列挙 → 実験)

P10-4 の事象「PSRAM 有効化 → SD 初期化の SDMMC プローブで TG1WDT リブートループ」に
対し、仮説を列挙して切り分ける。最低限:

- **H1: ピン競合**。ESP32-S3 の octal PSRAM は GPIO35〜37 を占有する。
  `board_pins.cpp` の SD(SDMMC)配線と Waveshare の回路図を突き合わせる。
  競合なら SD を SPI モードへ切り替える、または該当ピンを避ける余地があるかを調べる。
- **H2: SPIRAM 初期化と SDMMC 初期化の順序 / タイミング**(`CONFIG_SPIRAM_BOOT_INIT`、
  遅延初期化の可否、`CONFIG_SPIRAM_IGNORE_NOTFOUND`)。
- **H3: 設定不整合**(`CONFIG_SPIRAM_MODE_OCT` / `CONFIG_SPIRAM_SPEED`、フラッシュと
  PSRAM の速度組み合わせ制約、ESP-IDF v5.5.1 の既知 issue)。
- **H4: WDT が SD ではなく別の初期化で発火している**(ログの最終行だけで SD と
  断定していないか、`CONFIG_ESP_TASK_WDT` の対象タスクを確認)。

各仮説に対して 1 実験を割り当て(P10 の E1/E2/E3 と同じ構造)、結果を表にする。

### go/no-go 基準(実装前に合意)

go の条件(すべて満たす):

- PSRAM 有効 + SD マウント + WASM アプリ起動が **連続 20 回の再起動で 20 回成功**
  (作業 3 のスクリプトで回す)。
- PSRAM 有効時、**既存アプリの回帰に劣化がない**(WARN/ERROR なし、largest free
  block は増えるはず。減る場合は原因を報告)。
- **タイミングへの影響**: `midi_loopback` の E1 で、PSRAM 有効時にクロック欠落・
  外れ値が増えない(PSRAM のキャッシュミスがディスパッチャに影響しないことの確認。
  L0 は internal 固定だが、他タスクの挙動変化で間接的に影響しうる)。
- PSRAM 16B ランダムアクセスのレイテンシ実測(P10-4 で未取得だった値)を記録
  (判定条件ではなく設計根拠表への追記)。

no-go の場合: 原因と「解消に必要な変更の規模」を記録し、§11-2 を「延期」のまま
更新する。go の場合: §11-2 を改訂し、本番反映(WAMR プールの PSRAM 移動、
`MALLOC_CAP_SPIRAM` の使い分け方針)を別フェーズの課題として `docs/status.md` に
記録する。**いずれの場合も本フェーズ末に main は no-PSRAM 構成に戻す。**

## スコープ外

- metronome の書き直し、L0/L1・Host API 12 関数の変更(Phase 13 以降)
- 移行ステップ 4〜6(旧クリック経路の置換・削除、`hostapi_midi_send` の副作用削除)
- PSRAM の本番反映(go 判定後の別フェーズ)
- アプリ内 UI 操作の自動化(設計案のみ)
- Linux ホスト側の画面キャプチャ・自動操作(check-workflow で持ち越し済み)
- Expression pedal / ADS1115、ブラウザホスト(Phase A)

## 実行環境に関する指示

**実機ビルド、Linux ホスト用ビルド、flash、monitor、カメラ撮影など、シェルで
実行するものはすべて herdr の pane を作成して実行すること。** 直接実行は行わない。
pane 構成、コマンド、タイムアウト値は `docs/workflow.md` に従い、pane 操作は
`scripts/hpane.sh` を使用する。セッション開始時に `docs/workflow.md` を通読すること。

flash 前に `esp32-monitor` の docker コンテナがシリアルポートを保持していないか
`docker ps` で確認する(既知の教訓)。作業 3 以降は自作の回帰スクリプトが
モニタペインを占有するため、スクリプト終了後のコンテナ残存も同様に確認する。

パーティション拡張(作業 1)の初回フラッシュは `idf.py erase-flash` を伴う可能性が
ある。**実行前にユーザーへ確認し、SD カードの内容には影響しないことを明記する。**

## 完了条件

- 作業 1〜4 の各完了条件を満たしている。
- `docs/results/phase12.md` に、パーティション表の前後、アプリカバレッジ表と削除
  判断、自動化の方式と回帰表、PSRAM の仮説・実験・go/no-go 判定が記録されている。
- 検証専用コード(`PHASE12_*_TEST`)が削除され、main は no-PSRAM 構成で残した
  アプリ全本の自動回帰に合格している。
- `docs/workflow.md`(自動回帰の手順)、`docs/lessons.md`(新たな教訓)、
  `docs/architecture.md` §9 / §11-2(PSRAM 判定の反映)、`docs/status.md` が
  更新されている。
- `git status --porcelain` がクリーンで、コミットは作業単位(英語メッセージ)。

## 報告フォーマット

1. 作業 1: パーティション表(前後)、残容量、NVS 等への影響
2. 作業 2: カバレッジ表と削除提案 → **ここで一度停止** → 実施後の回帰表
3. 作業 3: 事前調査(シリアル経路の実測結果、コマンド集合、判定方式)→ **停止** →
   実装後の自動回帰表(手動回帰との一致確認)
4. 作業 4: 仮説と実験の対応表、各実験の結果、go/no-go 判定と根拠、
   レイテンシ実測値
5. Phase 13 への申し送り(自動回帰の使い方、残容量、PSRAM の扱い)
