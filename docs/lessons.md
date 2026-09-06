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

- PSRAM は搭載(8MB octal)だが `CONFIG_SPIRAM` は**無効**が現行構成。有効化
  (OCT + 80MHz + 既定の `SPIRAM_USE_MALLOC`)すると PSRAM 自体は正常認識・
  確保できるのに、**SD カード初期化の SDMMC プローブ(`esp_vfs_fat_sdmmc_mount`)
  から戻らず TG1WDT リブートループ**になる。PSRAM 有効化はドロップイン変更では
  ない(10 P10-4)。
- MP3 再生と共存する常駐タスク(計測・ログ等)は audio_player タスク
  (優先度 3)より低い優先度にすること。優先度 4 のログダンプタスク+再生中の
  タッチ操作(ログ大量出力)の複合で可聴の音切れが出た実績あり(10 P10-1)。

## SD カード / PSRAM(Phase 12)
- **`allocate linear memory failed` が出たらまず SD の初期化経路を疑う。**
  この機体は普段 **SDMMC(`Speed: 20.00 MHz`)** でマウントされ、アプリ実行時の
  largest free block は 31,744。SDMMC が失敗して **SDSPI にフォールバック**すると
  (`sdmmc_init_ocr: send_op_cond (1) returned 0x107` → `falling back to SDSPI`、
  `Speed: 11.43 MHz`)、最大連続ブロックがちょうど 16,384 B 減って **15,360** になり、
  WASM の linear memory(約 20KB 連続)が確保できず全アプリが起動しなくなる。
  ファームウェアは同一でも起きるので、ヒープの起動時ログだけ見ても気づけない(12)。
- SD が SDMMC で初期化できない状態に入ったら、**USB を抜き差しして電源を落とす**。
  `idf.py monitor` の再起動は RTS/DTR のソフトリセットで **SD カードの電源は落ちない**ため、
  何度リセットしても復帰しない。SD プローブ中のリブートループを繰り返した後に
  この状態へ入った実績がある(12)。
- PSRAM 有効化で SD がハングする真因は **SDMMC プローブ**だった(ピン競合でも、
  PSRAM 由来バッファが DMA 経路に渡るのでもない。`SPIRAM_USE_CAPS_ALLOC` でも同じ失敗)。
  プローブを飛ばせば PSRAM 有効で 20 回連続起動する。ただし飛ばすと常に SDSPI 経路に
  なるので、上記の破綻とセットで考える必要がある(12)。
- **PSRAM を有効にしても largest free block は増えない。** internal free は
  64,276 → 106,763 と +42KB 増えるが、最大連続ブロックは 31,744 のまま同一
  (独立した 32KB DRAM 領域が与える構造的上限)。WASM linear memory の逼迫を
  緩和するには WAMR プール自体を PSRAM へ移す必要がある(12)。
- PSRAM の 16B ランダムアクセス実測(-Og): internal 2KB **362 ns/op**、
  PSRAM 4KB(キャッシュ内)**375 ns/op(+3%)**、PSRAM 256KB(キャッシュ超え)
  **874〜966 ns/op(約 2.5 倍)**。キャッシュに収まるなら実質同等で、超えても
  internal の負荷時最悪値 1385 ns/op(P10-4)より速い(12)。

## 実機テストの自動化(Phase 12)
- ESP32-S3 の `/dev/ttyACM0` は内蔵 USB Serial/JTAG。ESP-IDF の
  **secondary console は出力専用**なので、primary=UART0 の構成のままだと
  ホストから送った文字は stdin に届かない。コンソール設定を変えずに受けるには
  `usb_serial_jtag_driver_install()` + `usb_serial_jtag_read_bytes()` で
  USJ を直接読む。ドライバを入れてもログ出力は影響を受けない。応答は
  **`printf` ではなく `ESP_LOG` で出す**(stdout は primary console = UART0 へ
  出てしまい USB 側に現れない)。`idf.py monitor` はホストのキー入力をそのまま
  転送し、行末は **CR**(LF は来ない)。38 文字 × 5 行の連続送信でも欠落なし(12)。
- 常駐プロセス(モニタ)の完了待ちを **herdr のペイン出力**に対して行うと、
  スクロールバックに残る前回の実行の行に誤マッチする。実際に、モニタ再起動直後の
  `waitfor` が前回の起動ログに一致し「まだ起動していないのに起動した」と誤判定した。
  待ちは **`tee` が書くログファイルの「待ちを始めた行より後ろ」**に限定すること
  (`scripts/device-regress.sh` の `wait_line`)(12)。
- **「既知の差分」は発生条件まで詰めること。** mp3player の −44B は Phase 9c 以降
  「既知挙動」として扱われてきたが、実は **MP3 を再生したときだけ**現れる
  (無操作で 35 秒保持しても +0)。手動回帰で毎回出ていたのは、ユーザーが実行中に
  再生ボタンをタップしていたため。自動化して初めて条件が分離できた(12)。
- 恒久タスクは `xTaskCreateStatic` + 静的スタックで作る。シリアルコンソール
  (静的 3KB)を足しても**アプリ実行時の largest free block は 31744 のまま不変**
  だった(free heap の水準だけが静的分 −3832B、ドライバのリングバッファ分 −1016B
  移動)(12)。

## ホスト共通(Phase 11 で得たもの)
- 実機と Linux ホストで**同じロジックを二重に書かない**。L0/L1 は
  `shared/seq_core.c`(OS API を呼ばない移植可能な C)に置き、時刻源・排他・
  タイマ・ポート出力だけをフックで差し替える。「同一の .wasm が両ホストで
  同じ挙動」をコードレベルで保証でき、ブラウザホストへの移植点も 1 箇所に
  閉じる(11 ステップ 2)。
- `uart_write_bytes` は IDF ドライバ内部の `tx_mux`(セマフォ)で直列化される。
  **portMUX のクリティカルセクションから呼んではいけない**(セマフォ待ちが
  起きうるため不正)。逆に、複数タスクから呼んでもバイトは交錯しないので
  呼び出し側でロックを取る必要もない(11 ステップ 1)。
- Linux ホストのワンショット再アームに `SDL_AddTimer` は使えない
  (ms 分解能しかなく 20833µs のクロックグリッドを表現できない)。
  専用スレッド + `pthread_cond_timedwait`(CLOCK_MONOTONIC)を使う(11 ステップ 2)。
- SDL の音声コールバックは `host_sdl_init` の時点で走り始めるため、
  **`host_midi_init` / `host_seq_init` より先に呼ばれる**。コールバックから
  初期化前のモジュール(時刻源のエポック等)を触ると 0 除算で落ちる。
  準備完了フラグで無視すること(11 ステップ 2)。
- ホストの検証は「アプリ自身に合否を判定させ、結果を数値で表示・送出させる」と
  実機と Linux で同じ手順が使える。**判定条件そのものが誤っていることがある**
  ので、落ちたらまずホストではなく検証条件を疑う(11: `seq_flush_after` の後に
  `seq_filled_until() <= now_tick` を要求したが、キューが空だと現在 tick が
  返り時々刻々進むため実機で落ちた)。

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
- リモートデスクトップ経由のセッションでは `/dev/snd/seq` が
  `Permission denied` になり ALSA シーケンサが使えない。ホストはログ出力へ
  フォールバックして動作を続けるが、**1 バイトごとに stderr へ書くため
  タイミング測定は無効になる**(σ が 800µs 級に膨らむ)。MIDI の時間測定を
  するときはローカルセッションで実行すること(11)。
- 同じ環境で、SDL のウィンドウ生成が稀に失敗して `app started` に到達しない
  ことがある。1 回の失敗で回帰を不合格と判断せず、再実行して切り分ける(11)。

## SD カードのシード / 測定(Phase 13)
- **`seed: wrote <n>/<n> bytes` のログはデータが載ったことを保証しない。**
  新しい `.wasm` を焼いた後 `WASM module load failed: magic header not detected` が続き、
  SD を PC で見たら**サイズは正しい(3910 B)のに先頭 3585 B がゼロ**だった。
  起動のたびに内容不一致と判定されて再書き込みされるのに直らず、ソフトリセットでも
  復帰しない。`fsck.vfat -n` の指摘はボリュームラベルのみで **FS 構造は健全**だったので、
  FAT の破損ではなく書き込みが載っていない側の問題。**復旧は PC でカードをマウントして
  手動コピー**(コピー後はアンマウント→再マウントして md5 を照合すること。
  ページキャッシュ越しの照合では確認にならない)(13)。
- ALSA シーケンサで**カーネル側タイムスタンプ**を使うときは `snd_seq_open` を
  **`SND_SEQ_OPEN_DUPLEX`** で開く。`SND_SEQ_OPEN_INPUT` だとキュー開始イベントを
  送れずキューが走らないため、打刻が全件 0 になる(13)。
- **MIDI DIN の受信間隔が 320µs(1 バイト時間)より短かったら、それは配送側で
  まとめて届いたアーティファクトである。** UM-ONE 経由の受信で「50ms の空白 →
  3µs で 2 発」という並びを観測したが、クロック総数は期待値と完全一致していた
  (欠落ではない)。受信側の間隔だけで送信ジッタを判定しないこと(13、P10-5 と同じ理屈)。
- **テンポが動いている区間を固定の公称値で判定しない。** 外れ値判定は公称間隔の
  1.5 倍 / 0.5 倍なので、長押しでテンポを動かしている区間は「欠落」ではなく当然の
  変化として大量に引っかかる(実測で 2,061 件)。`--segments auto` で区間に分けてから見る(13)。
- 常駐プロセス(測定プローブ等)を herdr のペインに `send` で置くと、原因不明で
  落ちることがあった(開始 60 秒で停止した実績)。**長時間の測定はペインから
  切り離して起動し、ユーザーに操作を依頼する直前に生存確認する**(13)。
