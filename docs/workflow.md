# MidiAppBox 標準開発ワークフロー(リファレンス)

## 位置づけ

- 本書は、herdr ペイン経由のビルド / フラッシュ / モニタ / カメラ撮影という
  **確立済みワークフローの原本**であり、以下の用途で参照する:
  1. 新しいフェーズ指示書(docs/prompts/phaseXX.md)入力時の動作確認・デバッグのベース。
     フェーズ指示書は本書を参照し、フェーズ固有の差分だけを書けばよい。
  2. ワークフロー自体の一巡チェック(§4 のモードで実行)。
- 本書は 2 層に分かれる:
  - **§1 不変条件**: やり方を変えてはならない点。変更には必ずユーザーの承認が要る。
  - **§2–3 推奨手順**: 既定の具体的なやり方。改善してよいが、**実行前に**差分と理由を
    提示して承認を得ること。承認なしに別のやり方へ置き換えない(試行錯誤で
    別解を探ることも含めて禁止)。承認されて成功したら本書と CLAUDE.md を更新する。
- 役割分担: **CLAUDE.md は常時従う原則の要点、本書は herdr/hpane ワークフローの具体
  (ペイン構成・コマンド形・タイムアウト・初回セットアップ・手順)の原本**。
  herdr/hpane に関する記載は CLAUDE.md には重複させず本書に一本化する。
  両者が食い違う場合は作業を進めず、食い違い自体を報告すること。
  **セッションの最初に必ず本書を通して読むこと**(CLAUDE.md の指示)。

## §1 不変条件(変更にはユーザー承認が必要)

再現性を壊す典型は「待ち方・実行形式・環境の無断変更」である。以下は理由込みで
固定されており、一見改善に見える変更(例: 番兵方式をやめて `herdr wait output` で
ログ文言を直接待つ)が過去に失敗した実績に基づく(各項の根拠は docs/lessons.md と
docs/results/)。

1. **シェル実行はすべて `scripts/hpane.sh` 経由**。直接 Bash で実行しない。
2. **完了待ちは `hpane.sh run` の番兵トークン方式のみ**。`herdr wait output` を
   ビルドログの文言に直接マッチさせない(スクロールバック誤マッチ・文言揺れ・
   高速スクロール取りこぼしの実績あり)。exit code が成否、124 はタイムアウト。
3. **pane ID を記憶・再利用しない**。必ずラベルから毎回解決する(ID は非永続)。
4. **ペインに対話状態を持たせない**。docker 内作業も毎回一発コマンド。
   cwd ドリフトに注意し、ビルドは絶対パス+成果物のタイムスタンプ/シンボル確認をセットで。
5. **Docker イメージタグはリポジトリトップ README.md 準拠**。build と flash/monitor は
   同一タグ。devcontainer CLI(`devcontainer up`/`exec`)は使わない(README と同じ
   生イメージへの `docker run`+`docker exec` では通る同一ソース・同一 pin バージョンの
   managed component が、devcontainer CLI 経由のビルドだと `-Wignored-qualifiers` が
   `-Werror` 化されてビルド失敗する現象を確認済み。根本原因未特定)。
   非対話コマンドで `idf.py` を使うときは `bash -c 'source /opt/esp-idf/export.sh && ...'`
   で明示 source(`docker run ... bash -lc '...'` はログインシェル扱いで `~/.bashrc` を
   読まないため不可)。
6. **monitor は `PYTHONUNBUFFERED=1 ... | tee <ログ>` 方式**で常駐(`send`)させ、
   `waitfor` とログファイルで読む。monitor 再起動は既定でボードをリセットする点に注意。
7. **タイムアウトは §6.2 の既定値表に従う**。超過時は勝手に次へ進まず、
   `read` でログ確認 → 報告して停止。
8. **実機のタッチ操作はユーザーに物理操作を依頼する**(プログラム注入経路なし)。
   Linux ホストの UI クリック自動化(xdotool のマウスクリック)は信頼できないため
   使わない。ランチャー操作が不要なら単発実行モードで回避する。
   画面キャプチャの自動化(x11grab 等)はこの環境では未解決・スコープ外。
9. **キャプチャ出力は `captures/<タスク名>/`**(.gitignore 対象)。
   Zenn 素材として残すものは `~/ビデオ/zenn-phaseXX/` にコピー。
10. `.claude/settings.local.json` の permissions 追記が必要になったら、
    内容を提示してユーザーに依頼する(勝手に権限前提の手順へ変えない)。

## §2 基本ワークフロー(やるべきこと)

環境ごとの手順と、各手順で必ず確認する観点。順序は Linux → ESP32 を推奨
(安価な環境で先に問題を潰す)。

### 2.0 環境確認(セッション初回に一度)
- `ensure` の冪等性(再実行で同一 pane)と `run` の一発実行(echo テスト)を確認
  してから本作業に入る(具体コマンドは §3.0)。
- 環境そのものの初回セットアップ(ユーザーが一度だけ実施)は §6.3。

### 2.1 Linux ホスト
やること: ビルド → 単発実行モードで対象アプリを起動 → 動作確認 → 終了。
確認観点:
- ビルドが exit 0。
- ログに起動(`app started`)→ 単発モード表示 → 終了(`app stopped`)が一貫して出る。
- stderr に警告(`no free slot`、WARN/ERROR)が無い。
- プロセスが残留していない。

### 2.2 ESP32 実機
やること: ビルド → フラッシュ → モニタ常駐 → (検証が必要な場合)カメラ録画を
開始した上でユーザーに物理操作を依頼 → 録画停止・静止画 → ログ確認。
確認観点:
- ビルド・フラッシュが exit 0。モニタで `app_main` 到達。
- アプリ起動〜終了がシリアルログで一貫している。
- **free heap が開始時と一致**(`app: stopped (ok), free heap NNNN (at start NNNN)`、
  リークなし)。
- WARN/ERROR/`no free slot` が無い。既知の起動ノイズ(ボードリセット直後の
  `I2C transaction unexpected nack detected` 一連、Touch online 前)は無視してよい。

### 2.3 記録
- 実施結果は `docs/results/<対応するファイル>.md` に追記(冒頭に対応する指示書への
  参照)。ファイルが無ければ `docs/prompts/` の指示書名に対応させて新規作成する。
- キャプチャは `captures/<タスク名>/` に置き、`git status` に現れないことを確認。

## §3 推奨手順(既定の具体的なやり方)

以下は check-workflow / 7D / 8c で実績のあるコマンド列。`<repo>` はリポジトリの絶対パス。

### 3.0 環境確認

```bash
herdr pane list --workspace 1            # 初回のみ: JSON 構造の実物を確認
./scripts/hpane.sh ensure unix-build     # 再実行して同一 pane_id を確認
./scripts/hpane.sh run unix-build "echo hello" 10000
```

`hpane.sh` は herdr の JSON 構造をキー名に依存しない形で走査しているが、
`ensure` が正しい pane ID を返さない場合はパーサ部(VERIFY コメント箇所)を
実際の JSON に合わせて修正し、修正内容を報告すること(改修は §5 の手続き。
一巡チェックモード §4 の実行中はスクリプトを修正せず、報告して停止する)。

ビルドキャッシュ関連のエラー(例: `cmake` の
`CMakeCache.txt directory ... is different than the directory ... where
CMakeCache.txt was created`、`idf.py` の managed_components ハッシュ不一致等)が
出た場合は、該当する `build/` ディレクトリ等(いずれも .gitignore 対象の
生成物)をクリーンにしてから同じコマンドを再実行してよい。**毎回のクリーン
ビルドはしない**(このエラーが出たときだけの対処)。Linux ホストの場合の
クリーン例:

```bash
rm -rf hosts/linux/build
```

### 3.1 Linux ホスト

```bash
# ビルド(絶対パスで cwd ドリフトを回避)
./scripts/hpane.sh run unix-build \
  "cd <repo>/hosts/linux && cmake -B build && cmake --build build -j" 600000

# 単発実行モードで起動(常駐なので send。DISPLAY を付ける)
./scripts/hpane.sh send unix-build \
  "cd <repo>/hosts/linux && DISPLAY=:0 ./build/midibox_host ../../wasm-apps/metronome/metronome.wasm"
./scripts/hpane.sh waitfor unix-build "app started" 15000

# 数秒動作させたのち、ESC キー送信で終了(キー送信は信頼できる。クリックは不可)
xdotool search --name "MidiAppBox WASM host"   # → <window id>(複数ヒットすることがある)
# 複数ヒットした場合は無関係なウィンドウ(mutter-x11-frames 等の装飾ウィンドウが
# 誤って一致することがある)が混ざっていないか、対象 pid と突き合わせて確認する:
#   for w in <window id...>; do xdotool getwindowpid $w; done
#   pgrep -af midibox_host   # ここで得た pid と一致するものを選ぶ
xdotool key --window <window id> Escape

# 確認
./scripts/hpane.sh read unix-build 60          # app started → single mode → app stopped
pgrep -af midibox_host                          # 残留なしを確認(何も出ない)
```

### 3.2 ESP32 実機

**`docker run --rm`(都度起動)に統一する。** 持続コンテナ + `docker exec`
方式は、`entrypoint.sh` の gosu 降格(`docker run`)を経由しない `exec`
(root 実行)と混在すると `build.ninja`/`.ninja_log` 等の所有者が割れて
`Permission denied` を起こす実績があるため使わない(詳細は docs/results/phase08a.md)。
ビルド・フラッシュ・モニタすべてこの方式で統一する。

`managed_components/`(gitignore 対象)を「再取得可能なキャッシュ」と即断して
中身を確認せず `rm -rf` してはならない。ハッシュ不一致で `idf.py fullclean` が
保護的に停止した場合、削除前に該当ファイルの差分を確認すること(ローカル修正が
入っていた可能性があるファイルを不用意に削除してしまった実績あり)。

```bash
# ビルド(README のタグの生イメージを都度起動。export.sh を明示 source)
./scripts/hpane.sh run esp32-build \
  "docker run --rm -v <repo>:/workspaces/MidiAppBox -w /workspaces/MidiAppBox/src \
   ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:5.5.5 \
   bash -c 'source /opt/esp-idf/export.sh && idf.py build'" 1800000

# フラッシュ(--device=/dev/ttyACM0 --group-add <dialout gid> を付けた
# docker run --rm -it を都度起動。monitor 同様 -it 必須)
./scripts/hpane.sh run esp32-build \
  "docker run --rm -it -v <repo>:/workspaces/MidiAppBox -w /workspaces/MidiAppBox/src \
   --device=/dev/ttyACM0 --group-add <dialout gid> \
   ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:5.5.5 \
   bash -c 'source /opt/esp-idf/export.sh && idf.py -p /dev/ttyACM0 flash'" 300000

# モニタ(常駐: send + waitfor + tee)。ログはマウント配下(ホスト側 captures/
# 等)に出す。コンテナ内一時パス(/tmp 等)は --rm で消え、herdr の
# スクロールバックも高頻度ログですぐ埋まるため、ホスト側ファイルで確認する。
./scripts/hpane.sh send esp32-monitor \
  "docker run --rm -it -v <repo>:/workspaces/MidiAppBox -w /workspaces/MidiAppBox/src \
   --device=/dev/ttyACM0 --group-add <dialout gid> \
   ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:5.5.5 \
   bash -c 'source /opt/esp-idf/export.sh && PYTHONUNBUFFERED=1 idf.py -p /dev/ttyACM0 monitor | tee /workspaces/MidiAppBox/captures/<タスク名>/monitor.log'"
./scripts/hpane.sh waitfor esp32-monitor "app_main" 60000
```

長時間接続した `idf.py monitor` は `docker ps` 上 `Up` のままサイレントに
詰まる(実機からの新規出力を転送しなくなる)ことがある。実機自体は動作を
続けているため、`docker kill <container id>` で該当コンテナを落として
モニタを再起動すれば復旧する。`herdr pane send-keys <pane_id> "C-c"` は
効かないことがあるので、`docker ps` で確認して直接 kill する方が確実
(詳細は docs/results/phase08c.md)。

ユーザーが実機を触りながら現象を確認したい場合、`esp32-monitor` ペインに
直接フィルタ済みのライブログを出すと効率的
(`idf.py monitor | tee <保存用ログ> | grep --line-buffered -E "<pattern>"`)。
ホスト側ファイルを都度読み上げて報告するより、ユーザー自身がペインを見ながら
物理操作できる。

### 3.3 実機の動作検証(カメラ+人間操作)

```bash
# 録画開始(常駐: send)
./scripts/hpane.sh send camera "./scripts/cam-rec.sh captures/<タスク名>"
```

→ ユーザーに具体的な手順を提示して物理操作を依頼する
(例: 「ランチャーから metronome をタップ → START → 数秒後 STOP → 終了」)。
完了の返答を**待ってから**次へ進む。

```bash
# 録画停止(空文字送信 = Enter)
./scripts/hpane.sh send camera ""
# 静止画(必要ならユーザーに画面状態の保持を依頼)
./scripts/hpane.sh run camera "./scripts/cam-still.sh captures/<タスク名>" 30000
```

- 生成物は `ffprobe` で h264 / 正常な duration を確認。
- 動画・音声ずれは原因特定・修正済み(2026-07-20、check-workflow-routine 後の
  別タスク。詳細は scripts/cam-rec.sh 冒頭コメントと docs/results/av-sync-fix.md)。根本原因は
  ffmpeg が v4l2/pulse の 2 入力を**それぞれ自分の先頭時刻で 0 にリセット**し、
  音声が映像より系統的に約164ms 遅れて始まる相対差を破棄していたこと(結果として
  音声が早く再生される)。pulse 入力に `-itsoffset`(既定 0.16s、環境変数
  `CAM_AUDIO_DELAY` で調整可)を前置して補正、ユーザーの ffplay 判定で同期を確認。
  併せて `-thread_queue_size`/`-timestamps abs` も維持(キュー詰まり回避・両入力の
  時刻系統一)。

## §4 一巡チェックモード(routine)

本書の手順を「そのまま一巡実行して確認するだけ」の回として実行する場合の追加ルール:

- **変更してよいのは docs/results/check-workflow-routine.md への実施記録の追記のみ**。
  scripts/・CLAUDE.md・ソースコードは触らない。新ラベル追加・レイアウト変更もしない。
- 手順: §3.0 → §3.1 → §3.2 → §3.3(操作内容は「なにかしらのアプリを実行する」で
  よい。metronome 推奨)。
- 想定外の失敗はその場で修正・回避せず、報告して停止する。docs/lessons.md
  記載の既知対処のみ適用可(それでも解決しなければ停止)。
- 完了条件:
  - 全手順が hpane.sh 経由で完走。
  - `captures/<タスク名>/` に実機の動画+静止画。`git status` に `captures/` が出ない。
  - free heap 一致・警告なしを確認済み。
  - docs/results/check-workflow-routine.md に追記(指示書参照、ビルド/フラッシュ/
    モニタの成否、heap 確認、警告有無の要約)。
  - `git status --porcelain` の差分が上記追記のみ。コミットもそれのみ(英語)。

## §5 推奨手順を改善したくなったら

1. 実行前に、変更点(現行 → 提案)と理由・期待効果を提示して承認を得る。
2. 承認後に試し、成功したら本書 §3 を更新。新たな教訓は docs/lessons.md
   にも一行追加する。
3. 失敗したら元のやり方に戻し、試行と結果を docs/results/ の該当ファイルに記録する。
4. §1 の不変条件に触れる変更は、より慎重に: 過去の失敗実績(該当する教訓)を
   引用した上で、なぜ今回は成立するのかを説明すること。

## §6 環境定義(ペイン構成・タイムアウト・初回セットアップ)

### 6.1 ペイン一覧(ラベル固定)

全ラベルは **このセッション自身のタブ(プロンプトペインが属するタブ)の中に、
プロンプトを最上段・全幅(既定で高さ 35%)、その下を 2 列 x 3 行で分割配置**
される(`scripts/hpane.sh` が `herdr pane split`/`pane rename` でペイン単位のラベルを
解決・作成する。タブ単位ではなくペイン単位のラベルなので `herdr tab list` では見えない
点に注意)。`ensure`/`run`/`send`/`waitfor`/`read` の呼び出しインタフェースは
「ラベルごとに別タブ」だった旧方式・「作業ペインだけの共有タブ」だった旧方式(いずれも
check-workflow-routine で廃止)から変わらない。

```
プロンプト(全幅、既定で高さ 35%)
------------------------------------------------
esp32-build   | unix-build
esp32-monitor | zenn
camera        | screen
```

列はプロンプトペインから down split で作った `esp32-build` を左列ルート、
そこから right split した `unix-build` を右列ルートとし、各列内は真上のペインから
down split して積む。プロンプトとの高さ比率は `HPANE_PROMPT_ROW_RATIO`
(既定 0.35)で調整できる。作成後のペインサイズはユーザーが `herdr pane resize`
等で自由に変えてよい。全ラベルの一括操作には `hpane.sh ensure-all`
(まとめて展開)/`hpane.sh close-all`(まとめて閉じる。既に閉じているラベルは
スキップ)、単体には `hpane.sh close <name>` が使える。

| ラベル | 用途 | 実行形式 |
|---|---|---|
| `esp32-build` | ESP32 実機ビルド / フラッシュ | 常に docker を含む一発コマンド(§3.2) |
| `esp32-monitor` | シリアルモニタ(常駐) | `send` で起動、`waitfor` でログ待ち |
| `unix-build` | Linux ホスト(SDL)ビルド / 実行 | 一発コマンド |
| `camera` | カメラ撮影(ffmpeg / v4l2-ctl、`scripts/cam-rec.sh`/`cam-still.sh`) | `send`(常駐)+ `run`(単発) |
| `zenn` | Zenn ドキュメント作成関連 | 一発コマンド |
| `screen` | Linux ホスト(SDL ウィンドウ)の画面撮影用(`scripts/screen-rec.sh`/`screen-still.sh`)。**現状この環境では x11grab が機能せず未使用・将来検討** | 一発コマンド(保留) |

新しいラベルを増やす場合は事前にユーザーの承認を得ること。

### 6.2 タイムアウト既定値(ms)

| 操作 | timeout |
|---|---|
| ESP32 フルビルド | 1800000 (30分) |
| ESP32 インクリメンタルビルド | 600000 (10分) |
| フラッシュ | 300000 (5分) |
| Linux ホストビルド | 600000 (10分) |
| モニタのログ待ち | 30000〜60000 |

タイムアウトした場合は勝手に次へ進まず、`read` でログを確認して状況を報告し
停止すること(§1-7)。

### 6.3 初回セットアップ(ユーザーが一度だけ実施)

```bash
# herdr 公式エージェントスキルの導入(Claude Code が herdr 操作を正しく学ぶ)
npx skills add ogulcancelik/herdr --skill herdr -g

# ヘルパー配置
chmod +x scripts/hpane.sh
```

`.claude/settings.local.json` の permissions に以下を追加:

```json
"Bash(herdr:*)",
"Bash(./scripts/hpane.sh:*)"
```
