# check-workflow-routine: 開発ワークフロー一巡チェック(定期実施)

## 実施記録 (2026-07-20) — 完了

対応: `docs/prompts/check-workflow-routine.md`。docs/workflow.md §4「一巡チェック
モード」として、確立済みワークフロー(§3.0〜§3.3)を設計・スクリプト変更なしで
一巡実行し確認した回。出力先タスク名は `captures/check-workflow-routine/`、
実機操作は metronome。

**§3.0(環境確認)**: 共有タブが未作成の状態から、全 6 ラベル
(esp32-build/esp32-monitor/unix-build/camera/zenn/screen)を `ensure` し、
2 回叩いて同一 pane_id を返すこと(冪等性)を確認。`run` の一発 echo テストも
exit 0 で成功。

**§3.1(Linux ホスト)**: `hosts/linux` を `cmake -B build && cmake --build
build -j` でビルド(SDL2_ttf/SDL2_mixer あり、exit 0)。単発モードで
`metronome.wasm` を起動、`app started` を確認後数秒 tick させ、`xdotool key
Escape` で終了。ログは `app started` → `single mode: ...` → `app stopped` の順で
一貫、stderr 相当の警告なし。`pgrep -af midibox_host` はプロセス残留なし。
(補足: `xdotool search --name "MidiAppBox WASM host"` が `mutter-x11-frames`
プロセスの装飾ウィンドウにも誤って一致する事例を確認。`getwindowpid` で
対象プロセスの PID と突き合わせて実ウィンドウを特定した。)

**§3.2(ESP32 実機)**: `docker ps -a` で確認したところ、実行中コンテナは
devcontainer CLI 由来の UID remap 済みイメージ(`vsc-midiappbox-...-uid`)のみで、
README タグ(`ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:5.5.5`)の持続コンテナは
存在しなかったため、`docker run -d --name midiappbox-esp32-build -v <repo>:/
workspaces/MidiAppBox -w /workspaces/MidiAppBox <tag> sleep infinity` で新規に
起動し、`docker exec` でビルド(`idf.py build`、exit 0、`midi_app_box.bin` 生成)。
フラッシュ・モニタは CLAUDE.md 教訓どおり `docker run --rm -it
--device=/dev/ttyACM0 --group-add 20` の都度起動方式(dialout gid=20)で実施し、
両方とも exit 0 / `app_main` 到達を確認。チェック終了後、今回起動した持続
コンテナ(ビルド用・モニタ用)は `docker stop`/`rm` で片付けた。

**§3.3(実機動作検証)**: `camera` ペインで `cam-rec.sh
captures/check-workflow-routine` により録画開始 → ユーザーに「ランチャーから
metronome 起動 → START → 数秒 → STOP → 終了」を依頼 → 完了報告を受けて録画停止
→ `cam-still.sh` で静止画取得。`ffprobe` で動画が h264/aac, 1280x720, 49.65 秒の
正常な mp4 であることを確認、`ffmpeg` プロセス残留なし。シリアルログで
`launch: /sdcard/apps/metronome.wasm` → `app_init() = 0, free heap 53508, tick
loop start` → タッチイベント複数 → `app: stopped (ok), free heap 73972 (at
start 73972), largest block 31744` → `menu: 6 app(s) listed` を確認、**free
heap は開始時と完全一致(リークなし)**。

**観測した警告(1件、既知の起動ノイズ一覧にはなし)**: ボード起動直後に
`W (385) spi_flash: Detected size(16384k) larger than the size in the binary
image header(2048k). Using the size in the binary image header.` が出ていた。
アプリ起動・動作・終了・heap 確認には影響なく、パーティションテーブル上の
想定フラッシュサイズ(2MB)と実機搭載フラッシュ(16MB)の差に関する ESP-IDF
起動時の定型警告であり、今回のビルド/フラッシュ手順が原因で新たに出た
ものではないと考えられる(アプリ非依存、毎回起動時に出る想定)。CLAUDE.md
既知の起動ノイズ一覧(I2C nack 系、Touch online 前)には含まれていないため
念のため記録するが、一巡チェックの完了条件(WARN/ERROR/no free slot なし)の
判定には影響しないと判断した。

**完了条件確認**: 全手順が `hpane.sh` 経由で完走(§3.0〜§3.3)。
`captures/check-workflow-routine/` に実機の動画(`cam_rec_121459.mp4`)・
静止画(`cam_still_121558.png`)を確認、`git status --porcelain` に `captures/`
は現れない(.gitignore 機能)。free heap 一致・(既知外の 1 件を除き)警告なしを
確認。scripts/・CLAUDE.md・ソースコードへの変更なし、`git status --porcelain`
の差分は本追記のみ。

**手順の改善案(§5 に基づき報告のみ、本チェック内では未実施)**:
- README タグの持続コンテナ(`esp32-build` 用)が存在しない状態から一巡
  チェックを始めるケースを想定し、§3.2 の先頭に「持続コンテナが無ければ
  `docker run -d --name <container> ... sleep infinity` で起動する」手順を
  明記してはどうか。現状は `<container>` が既に起動している前提でコマンド例
  だけが書かれており、今回のように無い場合の対処が本文になく都度その場で
  判断する必要があった。
- `xdotool search --name "MidiAppBox WASM host"` が複数ウィンドウ ID を返し、
  うち 1 件が無関係な `mutter-x11-frames` プロセスの装飾ウィンドウだった
  (今回は `getwindowpid` で実プロセスと突き合わせて回避)。§3.1 の手順に
  「複数ヒットした場合は `getwindowpid` で対象プロセスの PID と照合する」旨を
  一行加えると、次回以降同じ切り分けをせずに済む。

検証エビデンス: `captures/check-workflow-routine/cam_rec_121459.mp4`、
`captures/check-workflow-routine/cam_still_121558.png`(いずれも `.gitignore`
対象、リポジトリには含まれない)。

## 改善案の反映 (2026-07-20)

上記「手順の改善案」2 件について、ユーザー承認を得て docs/workflow.md §5 の
手続きに従い反映した(スクリプト・herdr レイアウトの変更は伴わない、
ドキュメントのみの更新):

- **docs/workflow.md §3.2**: 持続コンテナ (`<container>`) が存在しない場合の
  起動コマンド(`docker run -d --name <container> ... sleep infinity`)を
  ビルド手順の先頭に追記。
- **docs/workflow.md §3.1**: `xdotool search` が複数ウィンドウ ID を返した
  場合に `getwindowpid`/`pgrep -af midibox_host` で対象を照合する手順を追記。
- **CLAUDE.md 教訓チェックリスト**: 上記 2 点をそれぞれ「herdr / ビルド」
  「Linux ホスト(SDL / GUI 自動化)」節に一行追加。

## 実施記録 (2026-08-23) — 完了

対応: `docs/prompts/check-workflow-routine.md`。docs/workflow.md §4「一巡チェック
モード」として、確立済みワークフロー(§3.0〜§3.3)を一巡実行し確認した回。
出力先タスク名は `captures/check-workflow-routine/`、実機操作は metronome。

**チェック着手前の準備(本チェックの一巡実行そのものではないため、コミット
分離のうえ実施。§4 の「scripts/・CLAUDE.md 不変」制約はチェック開始後にのみ
適用)**:
- ユーザー要望により herdr ペイン運用を「共有タブ1つに3列×2行」から
  「**セッション自身のタブに、プロンプトを最上段・全幅(既定で高さ35%)、
  その下2列×3行(左列: esp32-build/esp32-monitor/camera、右列:
  unix-build/zenn/screen)**」に変更(`scripts/hpane.sh` 改修、
  `ensure`/`run`/`send`/`waitfor`/`read` のインタフェースは不変)。実装前に
  `ensure` による全展開・`herdr pane close` による全閉じ(閉じるとタブ自体も
  自動消滅することを確認)を実地検証し、列の振り分け・ratio の意味
  (アンカー側が確保する比率)もユーザー承認を得た上で反映(コミット
  `758411e`)。
- 上記の動作確認のため `unix-build` を一度ビルドしたところ、
  `hosts/linux/build/` の `CMakeCache.txt` が旧リポジトリパス
  (`/home/wurly/project/esp32/MidiAppBox`)を指しており cmake がエラー
  (想定外の失敗のため一旦報告して停止)。ユーザー指示により
  「ビルドキャッシュ関連エラー時のみ `build/` 等をクリーンにして再実行する
  (毎回のクリーンビルドはしない)」旨を docs/workflow.md §3 に追記
  (コミット `9094637`)してから一巡チェックを最初からやり直した。

**§3.0(環境確認)**: プロンプトペインと同居する新レイアウトで `unix-build`
(ルートである `esp32-build` も再帰的に作成)を `ensure`、2 回叩いて同一
pane_id(`wB:p1H`)を返すこと(冪等性)を確認。`run` の一発 echo テストも
exit 0 で成功。

**§3.1(Linux ホスト)**: 上記の準備で判明した stale な `hosts/linux/build/`
を `rm -rf` してから `cmake -B build && cmake --build build -j` でビルド
(exit 0。`-Wformat-truncation` 系の既知警告のみ)。単発モードで
`metronome.wasm` を起動、`app started` を確認後数秒動作させ、`xdotool key
Escape` で終了(`xdotool search` が装飾ウィンドウ含む複数ウィンドウ ID を
返したため `getwindowpid`/`pgrep -af midibox_host` で対象を照合)。ログは
`app started` → `single mode: ...` → `app stopped` の順で一貫、stderr 相当の
警告なし。`pgrep -af midibox_host` はプロセス残留なし。

**§3.2(ESP32 実機)**: ビルド・フラッシュとも `docker run --rm`(ビルドは
マウントのみ、フラッシュ・モニタは `--device=/dev/ttyACM0 --group-add 20`)の
都度起動方式で実施し、両方とも exit 0 / `app_main` 到達を確認。

**§3.3(実機動作検証)**: `camera` ペインで `cam-rec.sh
captures/check-workflow-routine` により録画開始 → ユーザーに「ランチャーから
metronome 起動 → START → 数秒 → STOP → 終了」を依頼 → 完了報告を受けて録画停止
→ `cam-still.sh` で静止画取得。`ffprobe` で動画が h264/aac, 1280x720, 34.07 秒の
正常な mp4 であることを確認、`ffmpeg` プロセス残留なし。シリアルログで
`Audio_Init: free heap 156372 -> 109168` → `WASM: runtime ready (pool 49152
bytes), free heap 102780` → `app: app_init() = 0, free heap 38568, tick loop
start` → `app: stopped (ok), free heap 59032 (at start 59032), largest block
31744` を確認、**free heap は開始時と完全一致(リークなし)**。モニタ用の
`docker run --rm -it` コンテナはチェック終了後に `docker kill` で片付けた。

**観測した警告**: ボード起動直後の `W (392) spi_flash: Detected size(16384k)
larger than the size in the binary image header(2048k). ...` のみ
(2026-07-20 の前回実施記録と同一の、パーティションテーブル上の想定フラッシュ
サイズと実機搭載フラッシュの差に関する ESP-IDF 起動時の定型警告。アプリ
非依存で毎回起動時に出る想定であり、完了条件の判定には影響しないと判断)。
それ以外の WARN/ERROR/`no free slot` はなし。

**完了条件確認**: 全手順が `hpane.sh` 経由で完走(§3.0〜§3.3)。
`captures/check-workflow-routine/` に実機の動画(`cam_rec_143200.mp4`)・
静止画(`cam_still_143240.png`)を確認、`git status --porcelain` に `captures/`
は現れない(.gitignore 機能)。free heap 一致・(既知の1件を除き)警告なしを
確認。本チェック内(§3.0〜§3.3 の実行そのもの)では scripts/・CLAUDE.md・
ソースコードへの変更なし(レイアウト変更とビルドキャッシュ対処の文書化は
チェック着手前の準備として別コミットで実施済み、上記参照)。

**手順の改善案(§5 に基づき報告のみ、本チェック内では未実施)**: 特になし。
着手前に判明した問題(レイアウトの見づらさ、ビルドキャッシュエラー)は
いずれも着手前の準備でユーザー承認済みの手順として反映済みのため、
一巡実行自体からは新規の改善提案は出なかった。

検証エビデンス: `captures/check-workflow-routine/cam_rec_143200.mp4`、
`captures/check-workflow-routine/cam_still_143240.png`(いずれも `.gitignore`
対象、リポジトリには含まれない)。
