# Phase 6: MP3 プレーヤーの WASM アプリ化(Host API v0→v1)

課題定義・完了条件はユーザー指示(2026-07-12)による。tick モデル維持、
入力はイベントキュー型、v0 4 関数は不変、アプリ実装が API 設計を駆動する。

**実機検証の運用(Phase 6 から)**: Web カメラ(/dev/video0)で実機を撮影して
検証する。録画は `~/ビデオ/rec.sh`(露出・フォーカス自動適用、Enter で停止)。
herdr の撮影用 pane で起動し、空文字送信(=Enter)で停止する。
動画・写真は Zenn 記事の素材として `~/ビデオ/zenn-phase6a/` 等に残す。

## 6A 実施記録 (2026-07-12) — 完了

入力イベント API `hostapi_poll_event` を追加し、touch_demo.wasm で両ホスト検証済み。

イベント規約(ユーザー承認済み、ABI 凍結):

- `hostapi_event_t` = **12 バイト固定** `{u16 type, u16 param, i16 x, i16 y, u32 time_ms}`
  (LE、time_ms は now_ms と同一時基)。type: 1=TOUCH_DOWN, 2=TOUCH_UP。
  拡張は type 追加(アプリは未知 type を無視する契約)と param で行い、
  レコードサイズは変えない。
- `hostapi_poll_event(buf, len)->n`(WAMR シグネチャ `"(*~)i"`)。
  ホストは len/12 件を上限にキュー先頭から書き、残りは次回。
- キュー深さ 16、満杯は最古から捨てる。**DOWN 未配送の UP は捨てる**
  (アプリを起動したタップの UP がアプリに漏れる問題の対策)。
  キューはアプリ起動時に空、破棄で消滅。

実装の要点:

- 実機の捕捉点は**アプリスクリーンへの LV_EVENT_PRESSED/RELEASED コールバック**
  (hostapi.cpp)。アプリ実行中だけイベントが流れる経路切替が構造的に成立する。
  そのため `fill_rect` の lv_obj は CLICKABLE を解除してスクリーンへ素通しする。
  座標は `lv_event_get_indev`+`lv_indev_get_point`。
- 生産者(LVGL タスク)/消費者(wasm アプリ pthread)間は portMUX spinlock。
  Linux は単一スレッドなのでロックなし(main ループの SDL_MOUSEBUTTONDOWN/UP を
  app_running 時のみ push)。
- LVGL indev はポーリング ~33ms だが、1 サンプルでも押下を観測すれば
  PRESSED→RELEASED の両遷移が出るため、33ms 以上のタップなら DOWN/UP が揃う。
- touch_demo(1549B)は座標表示+DOWN/UP カウント+CLICK ボタン(音+色変化)。
  背景とタイトルバーの (x,y) キー重複に注意(retained モデルでは後勝ち置換)。

検証結果:

- Linux: xdotool の click(DOWN/UP 間隔 ~12ms)で down/up が同数で増加、
  座標正確、CLICK 音再生。demo/bars 回帰 OK。
- 実機: タップで座標追従(シリアルの BASIC P(x,y) と画面表示が一致)、
  連打後 down:11 up:11 で完全一致(タップ長 243ms の例も DOWN/UP 両方配送)。
  power_key 短押しでメニュー復帰、demo.wasm 回帰 OK。
  アプリ停止時 free heap 80,980→80,980(開始時と一致、リークなし)。
- 記事素材: `~/ビデオ/demo_172530.mp4`(フルテイク 4:43)、
  `~/ビデオ/zenn-phase6a/`(トリム版 phase6a_touch_verification.mp4 と静止画 6 枚)。

## 6B 実施記録 (2026-07-12) — 完了

hostapi_audio_play/ctrl/set_volume/get_state の 4 関数を追加(設計はユーザー承認済み)。
両ホストで同一の mp3player.wasm(1983B、6C でプレイリストに育てる土台)により
再生/一時停止/再開/停止/音量/自然終了(FINISHED)/再生中終了の全項目を検証。

**メモリの壁と対策(6B 最大のリスクが実際に発現)**:

- フル Audio_Init(esp-audio-player タスク)は **47,172 bytes** 消費。
  FATFS(sector 4096 × max_files 8 ≒ 38KB)と重なると largest block が
  15,360 まで細り、**mp3player.wasm の instantiate が
  「allocate linear memory failed」で失敗**した(linear memory は
  WAMR の shrunk memory で ~20KB 連続を system heap から取る)。
- 対策(ユーザー承認: A 案): **CONFIG_FATFS_SECTOR_512 + max_files 8→4**。
  MP3 128kbps=16KB/s に対し sector 512 で十分。適用後はアプリ起動時
  free 57,688 / largest 31,744 で安定動作。
- PSRAM 有効化(案 C)は「次のメモリの壁」用のロードマップ項目として温存。

実装メモ:

- 状態機械はホスト側で宣言的に管理(`s_audio_state`)し、自然終了だけ
  `Music_finished()` を get_state/ctrl 時に取り込む。FINISHED/ERROR は
  次の play か STOP まで保持。状態不整合のコマンドは -1(トラップしない)。
- パスはミュージックルート相対(実機 /sdcard/music、Linux ./sdcard/music)。
  先頭 '/' と ".." は拒否(6C の fs_list と同じサンドボックス境界)。
- ライフサイクル契約: `hostapi_audio_reset()`(無条件 Music_stop)を
  アプリ起動直前と破棄時に呼ぶ。Linux は `host_sdl_audio_reset()`。
- 検証用 MP3(アルペジオ 12 秒 96,801B, 22.05kHz mono 64kbps)を
  **ファーム埋め込み→ /sdcard/music/test.mp3 へシード**(SD 抜き差し不要)。
  seed_file は 100KB 級に備えチャンク比較へ変更。
- クリック音(直接 I2S 書き込み)と esp-audio-player は排他前提のまま:
  mp3player はクリック音を使わない。同時使用は将来の音源 API で整理。
- サイクルテストは mp3player.wasm +「再生中に停止」を毎サイクル実施する形に変更。

実測(実機, -Og, 160MHz):

| 項目 | 値 |
|---|---|
| Audio_Init(フル)のヒープ消費 | 47,172 bytes |
| FATFS sector 512 + max_files 4 の効果 | アプリ起動時 free 40,084 → 57,688 |
| mp3player 実行中 free heap | ~29,400(instantiate 後) |
| 起動→再生→停止 10 サイクル | 開始 65,892 → 1 回目 65,692(−200B は初回のみ)→ **2〜10 回目 65,692 / largest 31,744 で完全一定** |
| 停止後の playing フラグ | 全サイクル 0(音停止契約 OK) |

検証エビデンス: `~/ビデオ/demo_185836.mp4`(55 分、操作は最後 6 分)、
`~/ビデオ/zenn-phase6b/`(トリム版+静止画 8 枚)。起動失敗(メモリの壁)の
一部始終は `~/ビデオ/demo_183116.mp4`。
Linux は SDL_mixer で同一検証+mixer 無しビルドのエラーパス確認済み。

## 6C 実施記録 (2026-07-12) — 完了

`hostapi_fs_list(idx, buf, len)->n`(`"(i*~)i"`、-1 で終端)を追加し、
mp3player.wasm(2942B)を本命のプレーヤーに拡張。両ホストで全完了条件を検証済み。

- fs_list はホスト側に状態を持たず毎回 readdir で idx 番目を返す(数十曲想定)。
  63 バイト超の名前とサブディレクトリは除外。列挙順は readdir 順(ソートなし)。
- アプリ UI: リスト 6 行+▲▼スクロール+選択ハイライト。retained モデルの
  スロット収支は rect 15/16・text 15/16(選択ハイライトは行位置固定の rect の
  色変え、スクロールは text の置き換えで実現 — 移動なしなのでスロットを食わない)。
- 連続再生はアプリ側: tick で get_state==FINISHED を見て次 idx を play、
  末尾なら CMD_STOP。実機ログで自然終了→次曲まで 100ms。
- エラー 3 ケース確認: (a) 曲なし/dir なし → 「no mp3 files in music dir」表示
  (b) 壊れた MP3 → state: ERROR 表示・クラッシュなし(mpg123 が resync 失敗を報告)
  (c) 再生中の短押し終了 → 音停止(6B に続き 6C でも確認)
- 実機シード曲を 3 つに(test.mp3 12s + tune_down/tune_duo 各 6s・32kbps ~24KB)。
- 10 サイクル(mp3player v2 + 毎サイクル再生中停止): 2 回目以降
  **64,840 / largest 31,744 で完全一定、リークなし**。
  注: サイクルログの playing=1 は停止直後(~100ms)のサンプリングで、
  esp-audio-player の停止が非同期なため。停止自体は有効
  (以後 Playback finished が一切出ないことで確認)。
- 教訓(運用): `herdr wait output --match` は高速スクロール行を取りこぼす。
  完了検知は「新しいブートの安定した末尾状態」をポーリングで見る。

検証エビデンス: `~/ビデオ/demo_204721.mp4`(1:40)と `~/ビデオ/zenn-phase6c/`
(静止画 9 枚: タップ再生→自動次曲→末尾停止→PAUSED、Linux の 8 曲リスト/
スクロール/エラー 2 種)。

## 6D 実施記録 (2026-07-12) — 完了・Phase 6 完了

旧 MP3 デモモードを削除してランチャーに一本化し、hostapi_defs.h を v1 として整理。

- **削除**: Kconfig `MIDIBOX_WASM_DEMO`(分岐ごと)、`components/ui`(旧デモ専用)、
  app_main の #else 経路。main の REQUIRES は power_key/display/touch/audio のみに縮小。
  `MIDIBOX_WASM_CYCLE_TEST` は depends を外して存続(リーク検証用)。
- **hostapi_defs.h v1**: gfx / input / audio / fs / misc にグループ化し、共通契約
  (座標系、文字列規約、out-buffer 規約、負数エラー・非トラップ方針、
  ライフサイクル+オーディオ停止契約、アプリ起動時の初期状態)をヘッダに集約。
  v0 の 4 関数はシグネチャ・挙動とも不変(X-macro の並び替えのみ。登録は名前
  ベースなので ABI 影響なし)。
- **hello/bench の扱いを明確化**: app_tick を export しないテストモジュールで
  あり、ランチャーアプリではない。ランチャーは「app_init/app_tick not exported」
  をメニューに表示して優雅に拒否する(両ホストで確認)。`run_selftest`/`run_bench`
  はユーティリティとして存続(現在は未配線)。
- **最終回帰**: Linux はメニューから 6 アプリ(4 本動作+hello/bench の拒否)、
  実機は 4 アプリ起動→短押し復帰(各 free heap 57,672 で一定)+
  mp3player 再生中終了で音停止。エビデンス: `~/ビデオ/demo_224403.mp4`(実機)、
  スクリーンショット一式(Linux)。

