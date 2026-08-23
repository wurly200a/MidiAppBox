# Phase 9a: Host API `hostapi_midi_recv` の追加(タイムスタンプ付き MIDI 受信) (2026-08-16) — 完了

対応: `docs/prompts/phase09a.md`。Host API/ABI 変更ゲート(調査報告 →
承認 → 実装)に従い実施。

**調査報告で提示し、ユーザー承認を得た決定事項**:
1. **レコード ABI**: 16 バイトアラインド(`uint64_t timestamp_us` +
   `uint8_t byte` + `uint8_t _reserved[7]`)。9 バイト packed 案は不採用
   (既存 `hostapi_event_t` の「natural alignment・非 packed」前例に合わせ、
   Rust 側の unaligned access リスクを避けるため)。
2. **ホスト側バッファ構成**: 実機は UART イベントタスク→リングバッファ
   (深さ 256 レコード = 4KB、既存タッチイベントキューと同じ
   `portMUX` spinlock + 満杯時最古破棄の設計)。Linux は ALSA シーケンサの
   受信スレッド→同型リングバッファ。
3. **タイムスタンプ分解能**: UART RX の閾値/timeout は Phase 8c 検証済み
   設定から変更せず。単発 System Realtime メッセージ(Clock 等)はバイト
   単位に近い精度、無 gap の連続バイト(ランニングステータスなしの
   マルチバイトメッセージ)は同一代表時刻になる仕様として明文化。
4. **Linux ホスト側実装**: 当初たたき台は「0 件返すスタブ」だったが、
   ユーザー承認により **ALSA rawmidi(実際には ALSA シーケンサ snd_seq の
   DUPLEX)経由での実受信** を採用(Linux 単体でも UM-ONE からの実 MIDI IN
   検証が可能に)。
5. **リングバッファ深さ**: 256 レコード / 4KB(Phase 8c で確認済みの
   302 バイト SysEx 1 件を余裕を持って収容)。

**実装(承認済み方針どおり)**:
- `shared/hostapi_defs.h`: `hostapi_midi_recv_t`(16 bytes, LE, ABI 凍結)
  を追加、`hostapi_midi_recv(buf_ptr, buf_len) -> n`
  (`hostapi_poll_event` と同型、`"(*~)i"`)を `HOSTAPI_NATIVE_SYMBOLS` に
  追加。"midi" セクションのドキュメントコメントを拡充。
- 実機(`src/components/midi/midi.{hpp,cpp}`、`board_pins.hpp`):
  `PIN_MIDI_RX = GPIO15`(Phase 8c 検証済み設定を踏襲、RXD 非反転)。
  `uart_driver_install` を `rx_buffer_size=1024`・`queue_size=16` に拡張し、
  UART イベントタスク(`rx_task`)が `UART_DATA` 受信ごとに
  `esp_timer_get_time()` を打刻してリングバッファへ push。オーバーラン/
  フレーミングエラーは Phase 8c のダンプ実装と同じ扱い(ログ+flush)。
  `Midi_Recv()` が `native_hostapi_midi_recv`(`hostapi.cpp`)から呼ばれ、
  `hostapi_poll_event` と同じ「buf_len から上限件数を算出して drain」
  実装。`Midi_Reset()` でリングバッファも破棄するよう拡張。
- Linux(`hosts/linux/hostapi_midi.{h,c}`): `snd_seq_open` を
  `SND_SEQ_OPEN_OUTPUT` から `SND_SEQ_OPEN_DUPLEX` に変更し、"MIDI IN" 用の
  入力ポート(`WRITE|SUBS_WRITE`)を新設。既存の「名前に "UM-ONE" を含む
  ポートへ自動接続」ロジックを入力方向にも複製(`try_connect_source`、
  `snd_seq_connect_from`)。受信専用スレッド(`rx_thread_fn`)が
  `poll()` + `snd_seq_event_input` + `snd_midi_event_decode`
  (デコード用に 512 バイトの別 `snd_midi_event_t` インスタンスを新設)で
  受信し、デコードした生バイトをタイムスタンプ付きでリングバッファへ push。
  タイムスタンプは `SDL_GetPerformanceCounter`/`Frequency` 由来の起動基準
  単調増加 µs クロック(実機の `esp_timer_get_time()` とは epoch が異なるが、
  レコード間差分にのみ使う前提のため許容)。`host_midi_reset()` で
  リングバッファも破棄。ALSA 非搭載ビルドでは常に 0 件(スタブ動作を維持)。

**テストコード**: `#ifdef PHASE9A_RECV_TEST` ガードで実機
(`src/main/app_main.cpp`)・Linux(`hosts/linux/main.c`)双方に
`hostapi_midi_recv` 単体疎通確認タスクを一時追加(CMakeLists に
`target_compile_definitions` を一時追加して有効化)。検証後、コード・
CMakeLists の変更ともに完全に削除・復元(`git diff` でこれら4ファイルの
差分が 0 になることを確認済み)。

**検証結果**:
- Linux ホスト: `midibox_host` 起動時に ALSA seq が UM-ONE (client 20)
  へ自動接続(`midi: MIDI IN connected to ALSA seq 20:0 (UM-ONE / UM-ONE
  MIDI 1)`)。ユーザーが UM-ONE 接続の鍵盤から演奏し、Note On/Off
  (`90`/`80` + note + velocity、3 バイトとも同一タイムスタンプ)・
  Clock(`F8`、単発ごとに個別タイムスタンプ、間隔約20ms)を正しいレイアウトで
  受信(計 1086 行、うち 907 件が Clock)。タイムスタンプは全件単調増加
  (`sort -c -n` で検証)。ALSA DUPLEX 初期化・受信スレッド起動〜
  Escape キーでのクリーン終了(受信スレッドは 200ms poll ループで
  即座に停止)まで確認、プロセス残留なし。
- ESP32 実機: ビルド・フラッシュ後、モニタで
  `MIDI: MIDI IN ready (GPIO15, RX not inverted)` を確認。UM-ONE 接続の
  鍵盤からの演奏で Note On/Off・Clock を受信(計 3523 件)、レイアウト
  正常・タイムスタンプ単調増加、UART エラー(overflow/framing/parity)
  およびリングバッファオーバーフローは 0 件。起動直後に 1 バイトのみ
  `00`(t=438µs)を受信したケースが1回あったが、その後は継続的なノイズは
  発生しておらず、UART ドライバ初期化直後のピンマルチプレクサ切替に伴う
  一過性のものと考えられる(Phase 8c で確認された「フローティング RX の
  周期ノイズ」とはパターンが異なり単発のみ、継続監視でも再現なし)。
- 回帰確認: production ビルド(テストコード削除後)で再フラッシュし、
  ランチャーから任意アプリを3回起動→ホーム短押しで終了、いずれも
  `free heap 59032 (at start 59032)` で一致(リークなし)、WARN/ERROR/
  `no free slot` なし。Linux ホストも production ビルドで metronome.wasm
  を単発実行し `app started`→`app stopped` まで警告なし、プロセス残留なし。

**トラブルと教訓**:
- Linux ホストの `midibox_host` を `| tee <ログ>` でパイプすると、
  標準出力が非 tty になり C stdio がフルバッファリングに切り替わるため
  `app started` 等の出力が herdr の `waitfor` から見えるまで大きく遅延する
  (`docker run | tee` で既知の `PYTHONUNBUFFERED=1` と同種の問題)。
  `stdbuf -oL -eL ./build/midibox_host ...` で標準出力/エラー出力を
  行バッファに強制することで解消した。今後 Linux ホストをパイプ経由で
  実行する際は `stdbuf -oL -eL` を付ける。
- ESP32 の app パーティションは本 Phase の時点で空き 2%(`0x50e0` バイト)
  まで逼迫している(Phase 9a 追加分は数百バイト程度で新規の逼迫要因では
  ないが、今後さらに Host API を追加する際は残量に注意)。

**検証エビデンス**: `captures/phase09a/`(.gitignore 対象)に実機モニタログ
(`monitor.log`: 受信検証、`monitor_final.log`: production ビルド回帰確認)。
Linux ホスト側ログは `/tmp/phase9a_linux_recv.log` 等(作業用一時ファイル、
リポジトリ非対象)。

