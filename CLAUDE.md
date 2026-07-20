# MidiAppBox — WASM PoC

ESP32-S3-Touch-LCD-2.8 (Waveshare) ベースの音楽デバイスファームウェア。
「サンドボックス化された WASM アプリを組込みデバイスに配信する音楽プラットフォーム」の
成立性検証 PoC を進行中。

## ドキュメント構成(必読)

- **CLAUDE.md(本ファイル)**: 常時従うルールと現在地のみ。
- **docs/prompts/**: フェーズ指示書(課題定義・完了条件の原本)。
  フェーズ開始時にユーザーが指定するファイルを読み、その指示に従う。
  スコープ変更は本文を書き換えず末尾に「追記 (日付)」節を足す。
- **docs/dev-log.md**: Phase 0〜 の調査・計画・実施記録・実測値・トラブルの詳細。
  過去 Phase に関わる作業(API 変更、メモリ調整、回帰など)の前に該当 Phase を読むこと。
- **docs/zenn.md**: Zenn 連載の運用ルール・連載構成対応表。
  記事の作成・更新作業を行うときは、作業前に必ず全体を読むこと。
- **docs/poc-results.md**: Phase 4 の計測結果と所見。
- **docs/workflow.md**: 標準開発ワークフローの原本(不変条件と推奨手順)。フェーズ開始時・動作確認時に参照。

## 言語

- ユーザーとのやりとり(会話・報告・質問)は日本語。
- git のコミットメッセージは英語。

## 現在地 (2026-07-20 時点)

- check-workflow(docs/prompts/check-workflow.md)完了。herdr ペイン運用を
  「ラベルごとに別タブ」から「**共有タブ1つに3列×2行で分割配置**」に変更
  (`scripts/hpane.sh` 改修。`run`/`send`/`waitfor`/`read` のインタフェースは不変)。
  撮影スクリプトを `scripts/cam-rec.sh` 等に整備し、出力を `captures/`
  (.gitignore 対象)に集約。Linux ホストの画面キャプチャ(x11grab)・
  クリックによる UI 自動操作は本開発環境(Wayland/XWayland + GNOME)の制約で
  信頼できる形にできず、スコープ外として持ち越し(詳細は
  docs/prompts/check-workflow.md 追記節、docs/dev-log.md)。
- Phase 7(7A 予約発音 / 7B メトロノーム / 7B-fix DMA 二重クリック / 7C トーンパレット /
  7D テンポ1刻み・ボリューム調整)完了。詳細は docs/dev-log.md の Phase 7 節。
- Zenn 連載: 第 1〜7 回公開済み、第 8〜17 回はスケジュール公開設定済み(〜2026-07-28、詳細は docs/zenn.md)。
- 次の候補: 未定(Phase 8 のスコープはユーザー指示待ち)。

## 開発の進め方(このリポジトリでの作業ルール)

- 「小さいターゲットを定めて、テストし、次を計画する」の反復。各フェーズはビルドが通り
  コミット可能な粒度を保つ。
- 開発環境は **herdr**。シェル実行はすべて下記「シェル実行(要点)」と
  **docs/workflow.md** に従い `scripts/hpane.sh` 経由で行う。
- Phase 完了まで、ビルド・フラッシュ・モニタ確認を含めて確認なしで自律的に進めてよい。
  各ステップの結果はログとして **docs/dev-log.md** に残すこと。
  実施記録の冒頭には対応するフェーズ指示書(docs/prompts/phaseXX.md)への参照を書く。
- 実機検証は Web カメラ(/dev/video0)で実機を撮影して行う
  (`scripts/cam-rec.sh` / `cam-still.sh`。具体的な使い方は workflow.md §3.3)。
  作業確認用の一時キャプチャは `captures/`(.gitignore 対象)、Zenn 記事の素材として
  残すものは従来どおり `~/ビデオ/zenn-phaseXX/` にコピーする。
- 依存追加は最小限。追加時は本ファイル末尾「依存の記録」に理由を残す。
- 既存アプリ(demo / bars / mp3player / metronome / clicktest)の回帰を壊さない。
  (旧「MP3 デモモードで分岐」ルールは Phase 6D で解消済み。履歴は docs/dev-log.md 冒頭。)
  
## アーキテクチャ方針(決定済み)

- タイミングクリティカル層(オーディオ出力、描画ドライバ、将来の FM 音源)はネイティブ
  ホスト側。WASM アプリはロジックのみを持ち、ホスト API を叩く。
- WASM アプリは Rust / `wasm32-unknown-unknown`(WASI 不使用)。
- 実機ランタイムは WAMR。まず interpreter で動かし、AOT は後続フェーズ。

## シェル実行(要点)

シェルで実行するもの(ビルド、フラッシュ、モニタ、カメラ撮影、動作確認)はすべて、
直接 Bash ではなく `scripts/hpane.sh` 経由で herdr のペインで実行する。
**ペイン構成、コマンドの具体形、タイムアウト既定値、初回セットアップ、
セッション初回の確認手順の原本は docs/workflow.md。**
シェル実行を伴う作業の開始前に必ず読むこと。

常時従う要点(詳細と根拠は workflow.md §1・§6):

- 完了待ちは `hpane.sh run` の番兵トークン方式のみ。`herdr wait output` を
  ログ文言に直接マッチさせない。exit code が成否(124 はタイムアウト)。
  失敗・タイムアウト時は `read` でログを確認して報告し停止する。勝手に次へ進まない。
- pane ID を記憶・再利用しない(ラベルから毎回解決)。ペインに対話状態を
  持たせない(毎回一発コマンド)。新しいラベルの追加は事前にユーザー承認。
- Docker イメージタグはリポジトリトップ README.md 準拠。build と flash/monitor は
  同一タグ。devcontainer CLI は使わない。
- 推奨手順からの逸脱・改善は、実行前に提案して承認を得る(workflow.md §5)。

## 教訓チェックリスト(詳細な経緯は docs/dev-log.md の該当 Phase)

### メモリ(ESP32)
- 大きな静的バッファを足したら free heap に加え `largest_free_block` を必ず確認(5A, 6B, 7B-fix)。
- ヒープからの恒久確保(タスク等)は最大連続ブロックを分断する。恒久物は静的確保に(7B-fix)。
- WAMR プールは現在 **48KB**(実測消費 ~27.5KB)。Linux も parity で 48KB を維持(7B-fix)。
- FATFS は sector 512 + max_files 4(6B。sector 4096 は連続ヒープ ~38KB を要求し WAMR と衝突)。

### WAMR
- WASM 実行スレッドは pthread で作る(`os_self_thread()` が `pthread_self()` を呼ぶ)(P1)。
- `wasm_runtime_load` に渡したバッファは unload まで保持(fast-interp は in-place 書き換え)(P1)。
- component の Kconfig 既定は全部盛り。LIB_PTHREAD 有効のままだと
  `wasm_runtime_create_exec_env` が失敗する(P1)。
- .wasm は必ず `-zstack-size` を縮小(既定だと Rust はスタック 1MB を要求)(P0)。

### 実機運用
- SD シード後に magic 不一致が続いたら SD 側の FS 破損を疑う(手動コピー / 再フォーマット)(7A)。
- monitor 再起動は既定でボードをリセットする。`--no-reset` は `-p <port>` 指定必須(7A)。
- monitor は `PYTHONUNBUFFERED=1 ... | tee <ログ>` で落とし、ホスト側ファイルポーリングで読む(7A)。

### herdr / ビルド
- 完了待ちは hpane.sh の番兵方式のみ。ログ文言への `wait output` 直マッチは
  高速スクロール行を取りこぼす(6C)。
- pane の cwd ドリフトに注意。ビルドは絶対パス+成果物のタイムスタンプ/シンボル確認を
  セットで行う(7C)。
- herdr の pane_id/tab_id は `<workspace>:p<N>`/`<workspace>:t<N>` 形式(`\d+-\d+` ではない)。
  `tab list`/`tab get` は pane_id を含まないため `pane list` と `tab_id` で突き合わせる必要がある(7D)。
- `python3 - <<'PY' ... PY`(ヒアドキュメント)は stdin を script 本体で使い切り、
  パイプ入力を読めない。パイプ入力を読むワンライナーは `python3 -c` で渡す(7D)。
- ESP32 ビルド用コンテナと flash/monitor 用コンテナはイメージタグを揃える。
  同名イメージでも tag 違い(pull 時期違い)でトゥールチェーンパスが食い違い、
  他方の `build/` キャッシュに対して壊れる(7D)。flash/monitor には
  `--device=/dev/ttyACM0 --group-add <dialout gid>` を付けた `docker run --rm -it`
  が必要(`-i` のみだと `idf_monitor` が real TTY 要求で失敗)(7D)。
- herdr の pane は `herdr pane split <pane_id> --direction right|down` で分割生成でき、
  `herdr pane rename <pane_id> <label>` でペイン単位に(タブラベルとは別軸の)
  ラベルを付けられる。`herdr pane list` の各要素の `label` フィールドで直接解決できるため、
  複数ラベルを 1 タブに同居させて同時視認できる(check-workflow)。
- **`devcontainer up`/`devcontainer exec`(devcontainer CLI)が作る UID remap 済み
  イメージは ESP32 ビルドに使わないこと。** README と同じ生イメージへの
  `docker run` + `docker exec` では通る同一ソース・同一 pin バージョンの
  managed component が、devcontainer CLI 経由のビルドだと `-Wignored-qualifiers`
  が `-Werror` 化されてビルド失敗する現象を確認(根本原因未特定、check-workflow で回避)。
- `docker run ... bash -lc '...'` はログインシェル扱いで `~/.bashrc`
  (IDF の `export.sh` を source している)を読まない。非対話一発コマンドで
  `idf.py` を使うには `bash -c 'source /opt/esp-idf/export.sh && idf.py ...'`
  のように明示 source すること(check-workflow)。
- `managed_components/`(gitignore 対象)を「再取得可能なキャッシュ」と即断して
  中身を確認せず `rm -rf` してはならない。ハッシュ不一致で `idf.py fullclean` が
  保護的に停止した場合、削除前に該当ファイルの差分を確認すること
  (check-workflow で、ローカル修正が入っていた可能性のあるファイルを不用意に
  削除してしまった)。
- README タグの生イメージによる持続コンテナ(`esp32-build` 用)は自動では
  存在しない。`docker ps -a` で見当たらなければ `docker run -d --name <container>
  -v <repo>:/workspaces/MidiAppBox -w /workspaces/MidiAppBox <README タグ>
  sleep infinity` で起動してから `docker exec` する(check-workflow-routine)。

### Linux ホスト(SDL / GUI 自動化)
- この開発環境(Wayland + XWayland、GNOME/Mutter)では `ffmpeg -f x11grab` は
  ウィンドウ位置・画面原点いずれでも常に黒画面になり使えない。GNOME Shell の
  D-Bus `org.gnome.Shell.Screenshot.ScreenshotArea` も `AccessDenied` で
  未署名スクリプトから呼べない。画面キャプチャの自動化は未解決(check-workflow)。
- `xdotool` によるウィンドウ検索・ジオメトリ取得・キー送信(Escape 等)は機能するが、
  **マウスクリックの配信は不安定**(`getmouselocation` で狙った座標に一致していても、
  意図しない行に届く/どこにも届かないことがある。`windowactivate` や `sleep` を
  挟んでも解消せず)。ボタン/メニュークリックに依存する自動 UI 操作は現状信頼できない。
  ランチャー経由が必要なければ単発実行モード(`./build/midibox_host <app>.wasm`
  で直接起動)を使うとメニュークリック自体を回避できる(check-workflow)。
- `xdotool search --name "MidiAppBox WASM host"` は複数のウィンドウ ID を返す
  ことがあり、うち `mutter-x11-frames` の装飾ウィンドウが無関係に混入する
  ケースを確認。`xdotool getwindowpid <id>` と `pgrep -af midibox_host` の
  pid を突き合わせて対象ウィンドウを特定してから `key`/`Escape` を送ること
  (check-workflow-routine)。

# 依存の記録

| 依存 | 追加フェーズ | 理由 |
|---|---|---|
| `espressif/wasm-micro-runtime` (registry, 2.4.0 系固定) | Phase 1 | WASM ランタイム本体。registry 経由が既存ビルドフロー(managed_components + Docker + CI)と整合し追加コスト最小 |
| (Linux) WAMR vmlib, SDL2 | Phase 3 | Linux ホスト用。実機と同一ランタイムで API 登録コードを共有するため |
| (Linux) SDL2_ttf(任意) | Phase 5 後 | 実機(LVGL の AA フォント)に見た目を合わせるため。無ければ font8x8 にフォールバックするので必須依存ではない |
| (Linux) SDL2_mixer(任意) | Phase 6B | MP3 再生。対案 mpg123 直叩きはデコード後の PCM 出力経路(デバイス管理・ミキシング)を自作する必要があるのに対し、SDL_mixer は pause/resume/volume/終了フック(Mix_HookMusicFinished)が hostapi_audio_* の状態機械に 1:1 で対応し、既存 SDL2 と同居できる。無ければ audio_play が常に ERROR を返すビルドになる(必須依存ではない) |

# ビルドメモ

- Docker: `ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5`(README 参照)。
  `esp32-build` ペインで一発コマンドとして実行。
- `src/` で `idf.py build` / `idf.py flash`。
- CI: `.github/workflows/build.yml` が devcontainer で `idf.py build`。
