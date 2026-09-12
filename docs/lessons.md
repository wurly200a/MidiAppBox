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
- PSRAM 有効化で SD がハングするのは **SDMMC プローブ**の中である(ピン競合ではない)。
  プローブを飛ばせば PSRAM 有効で 20 回連続起動する。ただし飛ばすと常に SDSPI 経路に
  なるので、上記の破綻とセットで考える必要がある(12)。
  **【Phase 15 で一部訂正】** 「`SPIRAM_USE_CAPS_ALLOC` でも同じ失敗だったから
  PSRAM 由来バッファが DMA 経路に渡る筋は消えた」という論法は**成立しない**。
  `CAPS_ALLOC` は `heap_caps_malloc(..., MALLOC_CAP_DEFAULT)` を internal に留めない
  (下記 Phase 15 の項)ので、ドライバがその形で確保していれば PSRAM のバッファは渡りうる。
  **この仮説はまだ生きている**(15)。
- PSRAM を有効にしても `MALLOC_CAP_INTERNAL` の largest free block は 31,744 のまま
  増えない(独立した 32KB DRAM 領域が与える構造的上限)。internal free は
  64,276 → 106,763 と +42KB 増える(12)。
  **【Phase 15 で訂正】** ここから導いた「だから WASM linear memory の逼迫は緩和されず、
  WAMR プール自体を PSRAM へ移す必要がある」という結論は**誤り**。
  linear memory はプールではなく `os_mmap()` 経由で確保され、`CONFIG_SPIRAM=y` にした
  時点で **PSRAM へ移っていた**(だから internal の largest が動かなかった)。
  WAMR プールを移す必要はなく、`sdkconfig.defaults` の変更だけで足りる(15)。
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

## PSRAM 本番反映(Phase 15)
- **`largest free block` は WASM の可否を表す指標ではない。** linear memory は WAMR の
  プールではなく `os_mmap()` 経由で確保され、**確保先は PSRAM へ変わりうる**
  (`espidf_memmap.c` は `WASM_MEM_DUAL_BUS_MIRROR` が立つと `MALLOC_CAP_SPIRAM` を使う)。
  判定は「実際に大きい `.wasm` が起動するか」と「`memory_data` のアドレス」で行う
  (0x3c/0x3d 台 = PSRAM、0x3fc 台 = internal DRAM)。**Phase 12 が「PSRAM を有効にしても
  largest が 31,744 のままなので効果なし」と判定したのはこの取り違えで、実際には
  linear memory はその領域から出て行っていた**(15)。
- **`CONFIG_SPIRAM=y` だけで WAMR の linear memory は PSRAM へ移る。** IDF のリネーム機構が
  旧名 `CONFIG_ESP32S3_SPIRAM_SUPPORT` を値付きで `sdkconfig.cmake` に出力し、WAMR の
  `shared_platform.cmake` がそれを見て `-DWASM_MEM_DUAL_BUS_MIRROR=1` を付けるため。
  `managed_components/` の書き換えは不要(15)。
- **`.wasm` の `--initial-memory` を増やしても linear memory は増えない。** WAMR の
  `WASM_ENABLE_SHRUNK_MEMORY`(既定 1)が `memory.grow` を含まないモジュールの宣言を無視して
  `num_bytes_per_page = align8(__heap_base)` に潰すため。実サイズは
  **`align8(__heap_base) + instantiate の heap_size`** で決まり、増やす操作は Rust 側の
  **`-C link-arg=-zstack-size=N`**(または大きな static)である(15、5 アプリで実測一致)。
- **`CONFIG_SPIRAM_USE_CAPS_ALLOC` は `heap_caps_malloc(..., MALLOC_CAP_DEFAULT)` を
  internal に留めない。** `malloc()` の実体 `heap_caps_malloc_default()` だけが
  `MALLOC_CAP_INTERNAL` を足して呼び直す実装で、`MALLOC_CAP_DEFAULT` を**直接**指定した
  確保は PSRAM から取れる(LVGL 描画バッファがこれで PSRAM に移った)。
  「CAPS_ALLOC にしたから PSRAM は明示確保だけ」という理解は誤り(15)。
- **色バッファが PSRAM にあるとき `esp_lcd_panel_io_spi_config_t.flags.psram_dma_direct`
  を立てないと、spi_master が転送のたびに internal の DMA バッファを一時確保して memcpy する**
  (`setup_dma_priv_buffer()`)。実測でフラッシュ 1 回ぶん 19,200 B の internal を食い、
  `largest_int` が 20,480 B 下がった。立てると internal 消費が 60 B になり、フラッシュの
  **最悪値も 8,814µs → 1,701µs** に改善する(平均は 663→823µs と悪化するが裾が効く)(15)。
- **PSRAM 有効時は `esp_get_free_heap_size()` と `MALLOC_CAP_DEFAULT` の largest が
  PSRAM 込みの 8MB 級になり、「internal の逼迫」も「リーク」も表さなくなる。**
  回帰のログは internal / PSRAM の 2 系統に分けて出すこと。**linear memory が PSRAM から
  取られる以上、internal だけ見ていると PSRAM のリークに気づけない**(15)。
- **回帰の判定で「余裕の監視」と「リーク検出」を混ぜない。** 前者は下限しきい値
  (`largest_int` はアプリごとに違いうるので固定値一致にすると偽 FAIL を作る)、
  後者は開始→終了の差分の厳密一致。Phase 14 までの `EXPECT_LARGEST` 固定値一致は
  この 2 つを混ぜていた(15)。
- **4 値ログをスクリプトで読むときは、同じ語が 1 行に 2 回出ることに注意。**
  `free_int=` は前半と末尾の `[start …]` の両方にあり、`sed` の `.*` は貪欲なので
  切り分けずに読むと**最後の出現 = 開始値**を拾い、差分が常に 0 になって
  **リーク検出が黙って無効化される**(15)。
- **「SDMMC プローブがハングする」という Phase 12 の理解は不正確だった。** 300ms の
  遅延を挿入すると SDMMC プローブ自体は正常にタイムアウト失敗して SDSPI へ
  フォールバックする。**真のハング地点は SDSPI フォールバックの SD プロトコル
  ネゴシエーション中(CMD5 応答直後)であり、SDMMC のネイティブプロトコルとは
  無関係。** 真因は「同一物理ピンを SDMMC ペリフェラルとして初期化した直後に
  SPI3 ペリフェラルとして再初期化する 2 段階遷移」が PSRAM 有効時に不安定になる
  ことで、SDMMC 単体の速度・ピン競合の問題ではない。SDSPI 単体(現行 main の経路)
  では一度もこの問題が起きていない(15 ステップ4)。
- **PSRAM 領域には `MALLOC_CAP_DMA` が登録されない**
  (`esp_psram.c` の `heap_caps_add_region_with_caps` は `MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT`
  のみ付与)。したがって `heap_caps_malloc(..., MALLOC_CAP_DMA)` を明示要求する
  確保(SD カードスタックの CID/CSD/response バッファ等)は `CAPS_ALLOC` でも
  `USE_MALLOC` でも PSRAM に流れようがない。「PSRAM 由来バッファが DMA 経路へ」
  という仮説を検証するときは、まず該当コードが実際にどの cap で確保しているか
  (`MALLOC_CAP_DMA` か `MALLOC_CAP_DEFAULT` か)をソースで確認すること(15 ステップ4)。
- **gitignore 対象の生成物(`sdkconfig` 等)を一時的に手動編集したら、
  `git diff`/`git status` だけで「元に戻った」と判断しないこと。** `idf.py build` は
  `kconfgen` で `sdkconfig` 全体を正規化し、`CONFIG_X=n` のような手書き形式を
  `# CONFIG_X is not set` に書き換えることがある。この正規化後に当初の sed
  パターン(`=n` を探す)が一致せず、値が意図せず残ったまま次のビルドに使われた
  実績がある。**復旧確認は必ずビルド成果物側**(`build/config/sdkconfig.h` の
  `#define` 行)で行うこと(15 ステップ4)。

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

## 旧 API 削除・実機単体測定(Phase 14)
- **実機単体で off-screen 統計を読みたいときは、Phase 9c の手法(画面外センチネル
  座標への `draw_text` を native 側の `#ifdef PHASE14_*_TEST` 等で一時的に
  `ESP_LOGI` へ転送)がそのまま再利用できる。** WASM アプリ側は常にこの座標へ
  描画し続けるだけで無害、native 側の分岐を外せば検証専用コードは跡形なく消える
  (`git diff` で変更前と一致することまで確認できる)。外部 PC・UM-ONE なしで
  「実機単体で回せる測定」を作るときの標準手段にしてよい(14)。
- **off-screen 統計だけでは足りないことがある。** 「clocks / expected」比のような
  画面表示専用の値は、STOP 後も画面を見ていれば読めるが、ユーザーが画面を
  戻すと失われる。**測定に必要な値は最初から全部シリアルログに出す設計にする**
  (今回は `dump_stop_stats()` に 1 行追加して再測定が必要になった)(14)。
- **削除系の Bash 操作(`rm -rf` / `git rm -r` 等)は、事前に安全タグ(git tag)を
  打っていても自動モードの分類器にブロックされる。** ブロックされたら
  ワークアラウンドを試みず、ユーザーに理由(指示書での明示・タグによる復元可能性)
  を説明して確認を取ること(14)。
- **Linux ホストの複数アプリを連続起動して `xdotool` で ESC 終了する自動化は、
  ウィンドウ ID とプロセス pid の対応付けにミスがあると `send` した後続コマンドが
  ペイン内でキューされたまま止まり、後から ESC が効いた瞬間に次々消化されて
  見かけ上「勝手に別アプリが起動する」ように見える。** ループの各ステップで
  「ESC を送った → 実際に `app stopped` が出た」ことを確認してから次へ進むこと
  (`pgrep` で残留有無を都度確認するのが安全)(14)。
- **副次的な発見**: `timeout N ./build/midibox_host <wasm>` で SIGTERM を送ると、
  SDL が `SDL_QUIT` イベントに変換するため `xdotool` の ESC 送信なしでも
  `app stopped` まで正常に完走する(5 アプリで確認)。ウィンドウ ID の解決が
  絡む `xdotool` 経由より単純だが、**§1 の不変条件(実行形式の無断変更禁止)に
  触れるため、workflow.md への採用はユーザー承認を経てから**(14)。
