# Phase 8a: MIDI OUT 疎通確認 (2026-08-12) — 完了

対応: `docs/prompts/phase08a_midi_out_bringup.md`。自作 MIDI OUT 回路
(2SC1815 トランジスタドライブ、GPIO18 = UART1 TX)が UM-ONE 経由で物理的に
正しく動作するかの確認のみ。Host API/ABI 変更なし、native host 側の
一時検証コードのみで完結。

**方針(承認済み)**: 独立ファイル `src/main/phase08a_midi_test.{hpp,cpp}` を
新規追加し、`app_main.cpp` には include + 起動呼び出しの2行のみ追加(hostapi.
cpp/hpp は不変)。独立 FreeRTOS タスク(優先度4、コア固定なし、UART1+GPIO18
のみ使用)で app_tick/audio/display と競合しない構成とした。送信内容は
Note On(0x90,60,100)→300ms→Note Off(0x80,60,0)→1500ms 待ち→ループ
(周期約1.8秒)。UART 初期化順は `uart_param_config` → `uart_set_pin`
(TX=GPIO18) → `uart_driver_install` → 直後に `uart_set_line_inverse`
(`UART_SIGNAL_TXD_INV`)。

**発生したトラブルと対処**:
1. **ビルド/フラッシュの UID 不一致**: 持続コンテナ (`docker exec`) はイメージ
   既定ユーザー(root)で実行され `build.ninja`/`.ninja_log` が root 所有に
   なる一方、フラッシュ用の都度起動 `docker run --rm`(entrypoint.sh の
   gosu ロジックでホスト UID 1000 に降格)がその root 所有ファイルへの
   書き込みで `ninja: build stopped: Error writing to build log: Permission
   denied` を起こした。`.gitignore` 対象の `src/build/` を削除し、以後
   build/flash/monitor すべてを `docker run --rm`(entrypoint 経由で UID
   統一)に統一して解消。**教訓: このイメージの entrypoint.sh は起動時
   カレントディレクトリ所有者に UID/GID を合わせて gosu で降格するが、
   `docker exec` はこれを経由せずイメージ既定ユーザー(root)で実行される。
   同一の持続コンテナに対して `exec`(root)と `run --rm`(gosu 降格後
   UID)を混在させるとビルド生成物の所有者が割れて権限エラーになるため、
   ビルドも `docker run --rm` に統一するのが安全。**
2. **`uart_driver_install` の `rx_buffer_size=0` がエラー**: `E (815) uart:
   uart_driver_install(1942): uart rx buffer length error` →
   `ESP_ERROR_CHECK` が `abort()`、パニック→リブートを繰り返した。IDF 5.5
   のレガシー UART ドライバは TX-only 構成でも `rx_buffer_size=0` を
   受け付けない(`UART_HW_FIFO_LEN` 超のバッファが必須)。`rx_buffer_size`
   を 256 に変更して解消(**教訓: IDF 5.5 で TX-only UART でも
   `uart_driver_install` の rx_buffer_size は 0 不可、最小限のバッファを
   明示確保すること**)。
3. **herdr pane の idf_monitor を止める操作**: `scripts/hpane.sh` の `send`
   はテキスト入力のみで割り込みキーを送れないため、`herdr pane send-keys
   <pane_id> "C-c"` で対処(`"C-]"` は `invalid_key` で拒否された。
   idf_monitor の正式な終了キーではないが、docker コンテナ自体を落とすには
   十分機能した)。

**実機検証**: 上記2点を修正後、ビルド・フラッシュ・モニタで起動ログに
`phase08a_midi_test: MIDI OUT test started (GPIO18, 31250bps, TXD
inverted)` を確認、クラッシュなくランチャーメニューまで到達。ユーザーが
UM-ONE + `aseqdump -p "UM-ONE"`(Linux ホスト、`aconnect -l` でクライアント名
確認)で目視確認。**初回は無反応(UM-ONE 受信 LED 不点灯)**→回路側の切り分けで
**自作回路のトランジスタの向きが逆**と判明、ユーザーが実装修正(はんだ付け
やり直し)。修正後、UM-ONE LED 点灯・`aseqdump` に Note on/off(ノート60、
ベロシティ100/0)が正しく表示され、数分間の連続観察でも化けたバイト・
表示途切れなしとユーザーが確認。

**完了後クリーンアップ**: 指示書どおり検証専用コードは確認後に削除する方針
とし、`src/main/phase08a_midi_test.{hpp,cpp}` を削除、`app_main.cpp`・
`src/main/CMakeLists.txt` の追加分を revert(`git status --porcelain` が
差分なしに戻ることを確認)。revert 後のクリーンな状態で回帰ビルド
(exit 0)を確認し、通常版ファームを再フラッシュ、モニタでランチャーメニュー
までの正常起動を再確認して終了。

**検証エビデンス**: 実機シリアルログ(herdr `esp32-monitor` ペイン
scrollback)。カメラ録画は本 Phase では実施していない(UM-ONE 側 MIDI
モニタでの目視確認が主眼のため)。

