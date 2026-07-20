# MidiAppBox 開発ワークフロー定期チェック(check-workflow-routine)

まず CLAUDE.md を読み、全ルール(シェル実行の原則、ペイン一覧、教訓チェックリスト)に
従うこと。本チェックの土台は `docs/prompts/check-workflow.md`(hpane.sh の共有タブ
分割化・撮影スクリプト整備を行った回)。その内容と末尾の追記節(Linux ホストの
画面キャプチャ・クリック自動操作がこの開発環境ではスコープ外になっている経緯)を
先に読んでおくこと。

## 目的

**既に確立済みのワークフロー(herdr ペイン経由のビルド / フラッシュ / モニタ /
カメラ撮影)を、そのまま一巡実行して動作確認するだけ**の回。
check-workflow.md はワークフローの土台自体(hpane.sh の分割方式化、撮影スクリプトの
新規作成)を作る回だったが、本チェックは**新しい仕組みを作らない**。

## スコープ

- **変更してよいのは `docs/dev-log.md` への実施記録の追記のみ**。
- **変更しない**: `src/`、`hosts/`、`wasm-apps/`、`shared/`、`scripts/`
  (`hpane.sh` を含む既存スクリプトの改修・新規スクリプトの作成は一切行わない)、
  `CLAUDE.md`(新しい教訓が判明した場合を除く。判明した場合も後述のとおり
  実装せず停止して報告し、ユーザーの指示を得てから追記する)。
  新しいペインラベルの追加、herdr レイアウトの変更も行わない。
- 設計・ファームウェア・App・Host API のソースコード変更は一切行わない。
- 既知の制約(Linux ホストの画面キャプチャ不可・クリック自動操作不安定)を
  再調査したり別解決策を試みたりしない。check-workflow.md 追記節に記載の
  「縮小済みの確認方法」をそのまま踏襲する。
- 想定外の失敗(ビルドエラー、ハッシュ不一致、ツールチェーン不一致等)が
  発生した場合、その場で調査・修正・回避策の実装はせず、状況を報告して停止する
  (CLAUDE.md「失敗時の扱い」どおり)。既知のトラブルシューティング
  (教訓チェックリスト記載の内容、例: `managed_components` のハッシュ不一致、
  `devcontainer` CLI を避け生イメージへの `docker exec` を使う、
  `bash -c` での `export.sh` 明示 source)は適用してよいが、それでも解決しない
  場合は停止する。

## 手順

### ステップ1: Linux ホスト(ビルド → 起動 → 終了確認)

1. `unix-build` ペインで `cd hosts/linux && cmake -B build && cmake --build build -j`
   (CLAUDE.md 既定タイムアウト)。
2. 単発実行モードで `./build/midibox_host ../../wasm-apps/metronome/metronome.wasm`
   を `send` で起動(`DISPLAY=:0` を付ける)。
3. 数秒 tick させたのち、`xdotool search --name "MidiAppBox WASM host"` で
   ウィンドウを特定し `xdotool key --window <id> Escape` で終了させる
   (単発モードでは ESC = quit)。
4. 確認: `app started: .../metronome.wasm` → `single mode: close window or
   press ESC to quit` → `app stopped` がログに出ていること。stderr に警告
   (`no free slot` 等)が無いこと。プロセスが正常終了していること
   (`pgrep -af midibox_host` で残っていないこと)。
5. 静止画・動画のキャプチャは行わない(check-workflow.md 追記節のとおりスコープ外)。

### ステップ2: ESP32 実機(ビルド → フラッシュ → モニタ → 人間操作の撮影)

- Docker イメージタグはリポジトリトップの README.md の記載に従う。
  **`devcontainer up`/`devcontainer exec` は使わない**(教訓チェックリスト参照。
  README と同じ生イメージのコンテナへ `docker run`/`docker exec` する)。

1. README のタグでコンテナを起動(未起動なら `docker run -d` 等で持続コンテナを
   立てるか、対話コンテナで作業してもよい。CLAUDE.md「実行形式のルール」に
   従い、可能な範囲で一発コマンド化する)。
2. `esp32-build` ペインでビルド(`bash -c 'source /opt/esp-idf/export.sh &&
   idf.py build'`、フルビルド 30 分タイムアウト)。
3. フラッシュ(同コンテナ、または `--device=/dev/ttyACM0 --group-add <dialout gid>`
   を付けた `docker run --rm -it` で `idf.py -p /dev/ttyACM0 flash`)。
4. `esp32-monitor` ペインで monitor を `send` 起動し、`waitfor` で `app_main`
   (タイムアウト 30〜60 秒)。ログは `PYTHONUNBUFFERED=1 ... | tee` 方式。
5. `camera` ペインで `./scripts/cam-rec.sh captures/check-workflow-routine` を
   `send` で開始した上で、**ユーザーに物理操作を依頼する**: 「ランチャーから
   metronome をタップ → START → 数秒後 STOP → 終了」を提示し、完了の返答を待つ
   (実機タッチはプログラムから注入できない)。
6. ユーザーの完了報告後、録画を停止し、`./scripts/cam-still.sh
   captures/check-workflow-routine` で静止画を1枚撮る。
7. 確認: シリアルログでアプリ起動(`launch: .../metronome.wasm`)〜終了
   (`app: stopped (ok), free heap NNNN (at start NNNN)`)まで一貫していること、
   **free heap が開始時と一致(リークなし)**であること、stderr 相当の警告
   (WARN/ERROR/no free slot/nack)が無いこと。

## 完了条件

- ステップ1・2が両方、既存の hpane.sh(共有タブ分割レイアウト)経由で完走している。
- `captures/check-workflow-routine/` に実機の動画+静止画が揃っている。
- `git status` に `captures/` が現れない(.gitignore が機能している)。
- `docs/dev-log.md` に実施記録を追記(冒頭に本指示書
  `docs/prompts/check-workflow-routine.md` への参照、結果の要約: ビルド/
  フラッシュ/モニタの成否、free heap 一致確認、警告有無)。
- **`docs/dev-log.md` 以外に差分が無いこと**(`git status --porcelain` で確認)。
- コミットは `docs/dev-log.md` の追記のみ。コミットメッセージは英語。
  (何かを変更する必要が生じた場合はコミットせず停止して報告)

## 失敗時の扱い

- `hpane.sh run` の exit 0 以外・タイムアウト(exit 124)は、`read` でログを
  確認して状況を報告し停止する。勝手に次のステップへ進まない(CLAUDE.md どおり)。
- 教訓チェックリストに無い新しい種類の失敗に遭遇した場合も、その場で修正・
  回避策を実装せず、状況を報告して停止する(本チェックのスコープは「実行して
  確認するだけ」であり、ワークフロー自体の改修は別タスクとする)。
