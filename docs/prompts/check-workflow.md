# MidiAppBox 開発ワークフロー一巡チェック(check-workflow)

まず CLAUDE.md を読み、全ルール(シェル実行の原則、教訓チェックリスト)に従うこと。
7D の運用実績(docker タグ問題、flash/monitor の起動形式、hpane.sh の修正経緯)は
docs/dev-log.md の Phase 7D 節を読んでから着手すること。

## 目的

7D で確立したワークフロー — herdr ペイン経由のビルド / フラッシュ / モニタ /
カメラ撮影 — を、**設計・ソースコードの変更なし**で一巡して動作確認し、
今後の開発のリファレンス(実質的な E2E チェック)とする。あわせて:

1. ペイン配置を「ラベルごとに別タブ」から「**1 タブ内の分割表示**」に変更する
   (全ペインを同時に視認できるようにする)。
2. 撮影系スクリプトを `~/ビデオ/` からリポジトリ内 `scripts/` に整備する。
3. 動画・静止画の置き場をプロジェクトツリー内 `captures/` に変更し、
   `.gitignore` でコミット対象外とする。

## スコープ

- **変更してよい**: `scripts/`、`CLAUDE.md`、`.gitignore`、`docs/`(dev-log 追記)。
- **変更しない**: `src/`、`hosts/`、`wasm-apps/`、`shared/`。
  設計・ファームウェア・App・Host API のソースコード変更は一切行わない。
  変更が必要と判明した場合は実装せず停止して報告。
- **既知の課題「録画の動画と音声のずれ」は本チェックでは修正しない。**
  現状の挙動のまま `scripts/` に移植する(修正は本チェック完了後の別タスク)。

## ステップ 0: ペイン配置の変更(1 タブ内分割)

- herdr スキルと CLI(`herdr --help` 等)を調査し、単一タブ内でペインを
  分割生成する API を確認する。
- `scripts/hpane.sh` の `ensure` を「共有タブ 1 つの中にラベル別ペインを
  分割で作る/既存ならそれを解決する」方式に改修する。
  `run` / `send` / `waitfor` / `read` のインタフェースは変えない。
- 検証: CLAUDE.md「初回導入時の確認事項」に準じ、`ensure` の冪等性
  (再実行で同一 pane)と `run` の一発実行(echo テスト)を全ラベルで確認する。
- herdr に分割 API が無い、またはペイン数・レイアウトに制約がある場合は、
  調査結果と選択肢を報告して停止すること(勝手に代替案へ進まない)。
- 完了後、CLAUDE.md のペイン表と運用記述を新配置に合わせて更新する。

## ステップ 1: 撮影スクリプトの整備

新ペインラベル **`screen`**(Linux SDL ウィンドウの録画・静止画用)を追加する。
CLAUDE.md の「新しいラベルは事前承認」ルールに対し、本指示書をその承認とする。
CLAUDE.md のペイン表にも追加すること。

作成するスクリプト(いずれも出力先を第 1 引数 `captures/<タスク名>/` で受け取り、
省略時は `captures/check-workflow/`。ファイル名は `<種別>_$(date +%H%M%S)` 形式):

| スクリプト | 内容 |
|---|---|
| `scripts/cam-rec.sh` | `~/ビデオ/rec.sh` の移植。v4l2 設定適用(録画開始後に当て直すウォッチドッグ含む)+ ffmpeg 録画。停止方法は現行踏襲(`camera` ペインで `send` 起動、空文字送信=Enter で停止)。A/V ずれは修正しない |
| `scripts/cam-still.sh` | v4l2 設定を当ててから `ffmpeg -f v4l2 -input_format mjpeg -video_size 1280x720 -i /dev/video0 -frames:v 1` で PNG 1 枚 |
| `scripts/screen-rec.sh` | `xdotool search` で SDL ウィンドウのジオメトリを取得し `ffmpeg -f x11grab` でウィンドウ領域を録画。停止は cam-rec.sh と同方式 |
| `scripts/screen-still.sh` | 同ジオメトリで静止画 PNG 1 枚(x11grab 1 フレーム、または ImageMagick `import -window`) |

- セッションが X11 でない(`$XDG_SESSION_TYPE` が wayland 等で xdotool /
  x11grab が機能しない)場合は、状況を報告して停止すること。
- `.gitignore` に `captures/` を追加する。
- スクリプトには実行権限を付与。`.claude/settings.local.json` への permissions
  追記が必要になった場合は、内容を提示してユーザーに追加を依頼する。
- **実装前チェックポイント**: ステップ 0 の hpane.sh 改修方針と、
  本ステップのスクリプト仕様(引数・出力先・停止方法)をまとめて提示し、
  承認を得てから実装する。承認後はチェック完走まで自律的に進めてよい。

## ステップ 2: Linux ホスト(ビルド → 実行 → 操作 → 撮影)

1. `unix-build` ペインでビルド(タイムアウトは CLAUDE.md 既定値)。
2. SDL ホストを起動(常駐なので `send`)。
3. `screen` ペインで `screen-rec.sh` を開始した状態で、`xdotool` により
   ランチャーから任意のアプリ(metronome 推奨)を起動し、数操作
   (START → 数秒 → STOP → 終了 程度)を行う。
4. 操作中に `screen-still.sh` で静止画も 1 枚以上取得する。
5. 録画停止 → アプリ終了 → ホスト終了。
6. 確認: stderr に警告(`no free slot` 等)がないこと。
   `captures/check-workflow/` に mp4 と png があり、mp4 は `ffprobe` で
   `h264` / 正常な duration を持つこと。

## ステップ 3: ESP32 実機(ビルド → フラッシュ → モニタ → 人間操作の撮影)

- **Docker イメージタグはリポジトリトップの README.md の記載に従うこと。**
  build と flash/monitor は同一タグで揃える(7D 教訓)。`build/` キャッシュが
  別タグ由来で不整合の場合は、README のタグで fullclean からビルドし直す。

1. `esp32-build` ペインでビルド(一発コマンド、フルビルド 30 分タイムアウト)。
2. フラッシュ: `docker run --rm -it --device=/dev/ttyACM0 --group-add <dialout gid>`
   形式(7D 教訓。`-i` のみは不可)。
3. `esp32-monitor` ペインで monitor を `send` 起動し、`waitfor` で `app_main`
   (タイムアウト 30〜60 秒)。ログは `PYTHONUNBUFFERED=1 ... | tee` 方式。
4. `camera` ペインで `cam-rec.sh` を開始した上で、**ユーザーに物理操作を依頼する**:
   具体的な手順(例: 「ランチャーから metronome をタップ → START → 数秒後 STOP →
   終了」)を提示し、完了の返答を待つ。実機タッチはプログラムから注入できない(7D)。
5. ユーザーの完了報告後、録画を停止し、`cam-still.sh` で実機画面の静止画を撮る
   (必要ならユーザーに画面状態の保持を依頼する)。
6. 確認: シリアルログでアプリ起動〜終了、および **free heap が開始時と一致
   (リークなし)** を確認する。

## 完了条件

- ステップ 0〜3 がすべて新レイアウト(1 タブ内分割)の hpane.sh 経由で完走している。
- `captures/check-workflow/` に Linux の動画+静止画、実機の動画+静止画が揃っている。
- `git status` に `captures/` が現れない(.gitignore が機能している)。
- docs/dev-log.md に実施記録を追記(冒頭に本指示書 `docs/prompts/check-workflow.md`
  への参照)。CLAUDE.md を更新(ペイン表への `screen` 追加と分割レイアウト、
  `~/ビデオ/rec.sh` 参照の `scripts/cam-rec.sh` への置き換え、現在地)。
  新たな教訓があれば教訓チェックリストに一行追加。
- コミットはスクリプト・.gitignore・ドキュメントの変更のみ(メディアは含めない)。
  コミットメッセージは英語。

## 失敗時の扱い

- `hpane.sh run` の exit 0 以外・タイムアウト(exit 124)は、`read` でログを
  確認して状況を報告し停止する。勝手に次のステップへ進まない(CLAUDE.md どおり)。

## 本チェック完了後の予定(本フェーズでは着手しない)

- cam-rec.sh の動画・音声ずれの修正(別タスクとして指示予定)。
