# 教訓チェックリスト

詳細な経緯は `docs/results/` の該当 Phase ファイルを参照(括弧内が Phase/回)。
herdr 運用・ビルド手順そのものの教訓は `docs/workflow.md` に一本化されている
(このファイルには含めない)。

## メモリ(ESP32)
- 大きな静的バッファを足したら free heap に加え `largest_free_block` を必ず確認(5A, 6B, 7B-fix)。
- ヒープからの恒久確保(タスク等)は最大連続ブロックを分断する。恒久物は静的確保に(7B-fix)。
- WAMR プールは現在 **48KB**(実測消費 ~27.5KB)。Linux も parity で 48KB を維持(7B-fix)。
- FATFS は sector 512 + max_files 4(6B。sector 4096 は連続ヒープ ~38KB を要求し WAMR と衝突)。
- WAMR プール(s_wamr_heap、native 側の固定 BSS)とは別に、native 側の
  一般ヒープ(FreeRTOS ヒープ)も WASM の linear memory 確保に影響しうる。
  native 側(hostapi.cpp/midi.cpp 等)に大きな静的バッファを追加すると、
  WAMR プール自体は変わらなくても一般ヒープの largest free block が縮小し
  "allocate linear memory failed" を誘発することがある(実機で largest
  free block が約15KBまで逼迫していた実績あり。Linux ホストでは同一の
  WAMR プールサイズでも再現しなかった点に注意)。追加前に `heap_init` ログ・
  `heap_caps_get_largest_free_block` で一般ヒープの余裕も確認すること(9c)。

## WAMR
- WASM 実行スレッドは pthread で作る(`os_self_thread()` が `pthread_self()` を呼ぶ)(P1)。
- `wasm_runtime_load` に渡したバッファは unload まで保持(fast-interp は in-place 書き換え)(P1)。
- component の Kconfig 既定は全部盛り。LIB_PTHREAD 有効のままだと
  `wasm_runtime_create_exec_env` が失敗する(P1)。
- .wasm は必ず `-zstack-size` を縮小(既定だと Rust はスタック 1MB を要求)(P0)。

## 実機運用
- SD シード後に magic 不一致が続いたら SD 側の FS 破損を疑う(手動コピー / 再フォーマット)(7A)。
- monitor 再起動は既定でボードをリセットする。`--no-reset` は `-p <port>` 指定必須(7A)。
- IDF 5.5 のレガシー UART ドライバは TX-only 構成でも `uart_driver_install` の
  `rx_buffer_size=0` を受け付けない(`uart rx buffer length error` →
  `ESP_ERROR_CHECK` で abort・パニックリブート)。RX を使わなくても
  `UART_HW_FIFO_LEN` 超の小さなバッファ(例: 256)を明示確保すること(8a)。
- フローティング(未接続)な UART RX ピンは、受信回路がトーテムポール出力
  (本来アイドル時は能動駆動で安定するはず)でも、配線漏れ等で実際には
  未接続だと周辺ノイズ(商用電源由来と推定される 50Hz 周期など)を拾って
  連続的に疑似バイト列を生成しうる。周期性のあるノイズは配線漏れの兆候として
  疑うこと。テスターは応答が遅く(数百ms〜)、UART 1バイト分の時間
  (31250bpsで約320µs)より短いノイズパルスを検出できない点にも注意(8c)。

## Linux ホスト(SDL / GUI 自動化)
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
- herdr の pane に `send-keys` で "Escape" を送っても `midibox_host`
  (SDL アプリ)は終了しない(ターミナルではなく SDL ウィンドウがフォーカスを
  持つため、キー入力はそちらに届く)。プロセスを止めるには
  `herdr pane send-keys <pane_id> "C-c"` でそのペインの前面プロセスへ
  SIGINT を送ること。放置すると同じペインへの後続コマンドが軒並み
  タイムアウトする(8b)。
