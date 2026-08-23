# Phase 5: SD ロードとランチャー (2026-07-05〜)

目的: /sdcard/apps/ の .wasm をメニューから選択起動し、power_key 短押しで
メニューに戻る(ユーザー選択済み)。起動↔終了 10 サイクルでリークなしが完了条件。

## 決定事項

- 「メニューへ戻る」= **power_key 短押し**(ユーザー選択)。現状 power_key は
  外部電源時にキーを監視していない(battery_mode のみ)ので、外部電源でも
  ポーリングして短押しイベントを拾う拡張を行う(長押し 2s 電源断は電池時のみ、従来どおり)。
- アプリライフサイクルはホスト所有: `app_start(path, on_stopped)` →
  100ms tick → `app_request_stop()` → (export されていれば) `app_exit()` →
  exec_env → instance → module の順に破棄 → バッファ free。
  ランタイム(`wasm_runtime_full_init`)は起動時に一度だけ(`runtime_init()`)。
- 初回セットアップ: /sdcard/apps が無ければ作成し、埋め込みのサンプル .wasm を
  シードする(以後は SD 上のファイルが正)。

## 5A 実施記録 (2026-07-05) — 完了

SD 上の /sdcard/apps/demo.wasm のロード・実行を実機確認。

**重要な障害と根本原因(再発注意):** ランチャービルドで SD マウントが
`mount_to_vfs failed (0x101=NO_MEM)` で失敗した。カードは SPI で応答しており
(cmd52/cmd5 の R1 ログ)、原因は **FATFS の VFS 登録が要求する連続ヒープ**
(`CONFIG_FATFS_SECTOR_4096` × `max_files=8` → FIL バッファ込みで ~38KB の
一括 calloc)に対し、WAMR の 128KB 静的プール(BSS)がヒープを圧迫して
最大連続ブロックが 31.7KB しかなかったこと。**WAMR プールを 64KB に縮小**
(Phase 4 実測 27.5KB 消費なので余裕)して解決。MP3 モードで動いていたのは
プールがリンカ GC で消えて余裕があったため。
教訓: 大きな静的バッファを足したら `heap_caps_get_largest_free_block()` も見る。

- SDMMC ホストは現在この個体でタイムアウト(263)し SDSPI フォールバックで
  マウントしている(MP3 モードも同じ)。以前は SDMMC で通っていたことがあり、
  ハード状態依存。フォールバックがあるので実害なし。
- マウントは 400ms 間隔で 3 回リトライ(初回タイムアウト対策)。

## 5B 実施記録 (2026-07-05) — 完了

メニュー(`launcher.cpp`)から demo.wasm / bars.wasm のタッチ起動を実機確認。

- メニュー UI: リスト行は lv_button+lv_label。行データはラベルテキストを
  そのまま使い(cb で `lv_label_get_text`)、rebuild ごとの動的確保なし。
  再スキャンは `launcher_show()` のたびに実施。
- wasm モードでも Touch を初期化するようにした(app_main)。
- **2 つ目のサンプルアプリ `wasm-apps/bars/`(640B)**: イコライザ風 8 本バー。
  課題どおり「跳ね回る矩形」も検討したが、ホスト API v0 の (x,y) キー retained
  モデルでは移動アニメがスロットを食い潰すため、**座標固定・サイズ/色可変**で
  アニメする設計にした(retained モデルでは縮小領域は LVGL の再描画で背景に
  戻るため消し込み矩形も不要)。この制約は API v1 検討時の材料。

## 5C 実施記録 (2026-07-06) — 完了・Phase 5 完了

- **メニュー復帰 = power_key 短押し**: PowerKey に短押しコールバックを追加
  (外部電源でもポーリング、長押し 2s 電源断は電池時のみ従来どおり。
  電池起動時の押しっぱなしを誤検知しないよう「解放を一度観測してから」計上)。
  コールバックは power_key タスク(小スタック)上なので atomic の
  `app_request_stop()` のみ。実機で 起動→短押し→メニュー→別アプリ起動 を確認。
- **リーク検証(CONFIG_MIDIBOX_WASM_CYCLE_TEST=y で自動実行)**:
  demo.wasm の起動 2 秒→停止を 10 サイクル。free heap は
  開始 91,712 → サイクル 1 後 91,460(−252B はメニュー初回構築の一度きり)→
  **サイクル 2〜10 まで 91,460 / largest 34,816 で完全一定。リークなし。**
  WAMR の deinstantiate→unload→(プール再利用) が正しく回ることを確認。
- **エラーハンドリング**: SD 未挿入 → メニューに「SD mount failed」表示で滞留
  (実機確認)。壊れた .wasm → 「magic header not detected」をメニューに表示し
  クラッシュなし(自動テストで確認)。apps 空 → 「no .wasm files」表示
  (コードパスは同一、目視は未実施)。
- 停止コールバックは **cb 実行後に Idle へ遷移**する順序にした(次アプリの
  画面生成と、cb 内のメニュー復帰・画面破棄の競合防止)。

