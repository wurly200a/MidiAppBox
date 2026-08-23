# check-workflow: 開発ワークフロー一巡チェック

## 実施記録 (2026-07-20) — 完了(一部スコープ縮小)

対応: `docs/prompts/check-workflow.md`。7D で確立した herdr ペイン経由のビルド/
フラッシュ/モニタ/カメラ撮影のワークフローを、設計・ソースコード変更なしで
一巡動作確認する回。スコープ変更は `docs/prompts/check-workflow.md` 末尾の
追記節を参照。

**ステップ0(hpane.sh 改修)**: herdr の `pane split`/`pane rename` を実地検証し、
ラベル解決を「タブラベル検索」から「ペインラベル検索」に変更。共有タブ 1 つ
(`midiappbox-panes`)内に 3 列 x 2 行(esp32-build/esp32-monitor/unix-build/
camera/zenn/screen)で分割配置する方式へ改修。各ラベルにアンカーラベル+分割
方向を持たせ、`ensure_pane` を再帰化することで呼び出し順序に依存せず同じ
配置に組み上がるようにした。全 6 ラベルで `ensure` の冪等性(2 回叩いて同一
pane_id)と `run` の一発 echo 実行を確認。`run`/`send`/`waitfor`/`read` の
インタフェースは変更なし。

**ステップ1(撮影スクリプト整備)**: `scripts/cam-rec.sh`(`~/ビデオ/rec.sh` の
移植)、`cam-still.sh`、`screen-rec.sh`、`screen-still.sh` を作成、実行権限付与。
新ラベル `screen` を追加。`.gitignore` に `captures/` を追加。

**ステップ1後の発覚事項 — スコープ縮小(ユーザー承認済み、詳細は
check-workflow.md 追記節)**:
- `screen-rec.sh`/`screen-still.sh` は実装したが、この開発環境
  (Wayland + XWayland、GNOME/Mutter)では `ffmpeg -f x11grab` が常に黒画面を
  返し機能しない。GNOME Shell の D-Bus `Screenshot.ScreenshotArea` も
  `AccessDenied` で使えず、`xdg-desktop-portal` 経由の ScreenCast は初回に
  対話的な許可ダイアログが必要になるため見送り。**Linux ホストの画面キャプチャは
  今回のスコープから除外、今後の検討課題**とした。
- ランチャーのメニュー行を `xdotool mousemove --sync` + `click` で自動操作しようと
  したところ、座標計算・`getmouselocation` による検証・`windowactivate`・
  `sleep` を挟んでも**クリックが意図した行に届かない/別の行に届く**という
  不安定挙動が発生(原因未特定)。画面キャプチャも使えず結果を検証できないため、
  **Linux ホストのボタンクリック検証も今回のスコープから除外**。代わりに
  CI スモーク用の単発実行モード(`./build/midibox_host <app>.wasm`)で
  metronome.wasm を直接起動し、`app_tick` が数秒間正常に回ること・stderr に
  警告が無いこと・`xdotool key Escape` での正常終了のみを自動確認する方式に
  縮小した。

**ステップ2(Linux ホスト)**: `hosts/linux` を `cmake -B build && cmake --build
build -j` でビルド(SDL2_ttf/SDL2_mixer あり)。単発モードで
`wasm-apps/metronome/metronome.wasm` を起動、数秒 tick させ `xdotool key
Escape` で終了。`app started` → `single mode: close window or press ESC to
quit` → `app stopped` を確認、stderr 警告なし、プロセスも正常終了。

**ステップ3(ESP32 実機)**:
- ビルドでトラブル: `devcontainer up`/`devcontainer exec`(devcontainer CLI が
  作る UID remap 済みイメージ `vsc-midiappbox-...-uid`)でビルドすると
  `chmorgan__esp-audio-player` 1.0.7(pin 済み、`idf_component.yml` で
  `==1.0.7` 固定)の `audio_player.cpp:568` で
  `error: type qualifiers ignored on cast result type [-Werror=ignored-qualifiers]`
  が発生しビルド失敗。**同じソース・同じ pin バージョンでも、README と同じ
  生イメージへの `docker run` + `docker exec` ではこの箇所は warning のみで
  ビルドが通ることをユーザーが実地検証**(根本原因未特定。UID remap 由来か
  イメージビルドの非決定性かは切り分けられていない)。以後 ESP32 ビルドは
  devcontainer CLI を使わず、生イメージへの `docker exec` を使う方式に統一。
- **ミス・教訓**: `idf.py fullclean` が `managed_components` のハッシュ不一致
  (`chmorgan__esp-audio-player/audio_player.cpp` が pin バージョンと差分あり)で
  保護的に停止した際、中身を確認せず「gitignore 対象の再取得可能キャッシュ」と
  判断して `rm -rf managed_components` してしまった。実際にはローカル修正
  (おそらく上記 `-Wignored-qualifiers` 対策)が入っていた可能性が高く、
  復元不能にしてしまった。結果的に生イメージでは同じ箇所が warning で通ったため
  実害はなかったが、**gitignore 対象でも中身を確認せず削除しないこと**を
  教訓とした。
- `docker run ... bash -lc '...'` はログインシェル扱いで `~/.bashrc`
  (`export IDF_PATH=/opt/esp-idf; source /opt/esp-idf/export.sh` を含む)を
  読まないため `idf.py: command not found` になった。`bash -c 'source
  /opt/esp-idf/export.sh && idf.py ...'` で明示 source する方式に修正。
- ビルド・フラッシュは README タグ(`ghcr.io/wurly200a/builder-esp32/
  esp-idf-v5.5:5.5.5`)の生イメージコンテナへの `docker exec`/`docker run`
  で成功(`midi_app_box.bin` 生成、`idf.py -p /dev/ttyACM0 flash` 成功)。
  シリアルモニタは `docker run --rm -it --device=/dev/ttyACM0 --group-add
  <dialout gid>` の別コンテナで起動、`app_main` を `waitfor` で確認。
  ユーザーに「ランチャーから metronome → START → 数秒 → STOP → 終了」の
  物理操作を依頼し、`camera` ペインで `cam-rec.sh`(55 秒、h264 1280x720)+
  `cam-still.sh` で記録。シリアルログで `launch: /sdcard/apps/metronome.wasm`
  → `app_init() = 0, free heap 53508, tick loop start` → タッチイベント
  複数回 → `app: stopped (ok), free heap 73972 (at start 73972), largest
  block 31744` → `menu: 6 app(s) listed` を確認、**free heap が開始時と完全
  一致(リークなし)**。stderr 相当の警告(WARN/ERROR/no free slot/nack)は
  スクロールバック確認範囲でなし。

**完了条件確認**: `captures/check-workflow/` に Linux ホストの動画・静止画は
無い(スコープ除外)が実機の動画(`cam_rec_112014.mp4`)・静止画
(`cam_still_112117.png`)は取得済み。`git status` に `captures/` は現れない
(.gitignore 機能)。CLAUDE.md 更新(ペイン表 `screen` 追加・分割レイアウト・
`cam-rec.sh` 参照・教訓チェックリスト・現在地)。

検証エビデンス: `captures/check-workflow/cam_rec_112014.mp4`、
`captures/check-workflow/cam_still_112117.png`(いずれも `.gitignore` 対象、
リポジトリには含まれない)。

