# Phase 8b: MIDI Clock 出力 Host API 追加 + メトロノームアプリへの実装 (2026-08-12〜13) — 完了

対応: `docs/prompts/phase08b_midi_clock_api.md`。Phase 8a で確認済みの MIDI OUT
回路(GPIO18=UART1 TX)の上に、Host API `hostapi_midi_send` を追加し、
メトロノームアプリの START/STOP に MIDI Start(0xFA)/Stop(0xFC)を相乗りさせ、
host 内部で 24ppqn の MIDI Clock(0xF8)を生成する。Host API/ABI 変更を含む
ため、指示書どおり投資方針レポート→承認→実装の順で進めた。

**承認された設計**:
- Host API は `hostapi_midi_send(bytes_ptr, bytes_len) -> 0/-1` の1つのみ
  (`shared/hostapi_defs.h` に "midi" セクションとして追記)。App は素の
  MIDI バイト列を送るだけで、host が単独の 0xFA/0xFB(Start/Continue)・
  0xFC(Stop)を検出してクロック生成を開始/停止する(App drives API)。
- 24ppqn クロックは新規のテンポ伝達経路を作らず、既存のクリック/トーン
  予約スケジューラ(`tone_schedule_impl`)の「予約時刻」から導出する。
- 実機: 新規コンポーネント `src/components/midi/`(`midi.hpp/.cpp`)を追加。
  UART1 初期化は起動時に1回(`app_main.cpp` から `midi::Midi_Init()`)。
  `hostapi.cpp` の `tone_schedule_impl`/`click_timer_cb` から
  `midi::Midi_NotifyBeatScheduled`/`Midi_NotifyBeatFired` を呼ぶ形で
  クリックスケジューラを「拡張」(既存フィールドの意味は変更しない)。
- Linux: `hosts/linux/hostapi_midi.{c,h}` を新規追加。ユーザーの選択で
  ALSA シーケンサ(snd_seq、新規依存・任意)経由で実際の外部 MIDI 機器
  (UM-ONE)へ送信(無ければ stderr ログのみにフォールバック)。
- メトロノーム(`wasm-apps/metronome/src/lib.rs`)は既存 START/STOP
  ボタンに `hostapi_midi_send([0xFA])`/`([0xFC])` を追加するのみ。

**テンポ導出ロジックの2回の設計改訂(重要)**:
1. 初版は「直前に**発音した**時刻」との差分でテンポを導出。実機での長時間
   テストで、テンポ変更(BPM+/-、拍子)を跨いだ瞬間にクロック間隔が
   一瞬 ms 単位まで暴走する不具合を発見(`rearm()` が「拍0を今すぐ」を
   再予約する際、host 側の通知が「直前拍からの経過時間」を見かけ上の
   周期として誤って staging してしまうため)。
2. 100ms 未満を弾く妥当性フィルタで応急処置したが、ロングプレス長押し
   連打(`process_repeat`、最速 100ms 間隔で `rearm()` が繰り返される)で
   閾値ちょうど付近の誤検出が再発することを実機再検証で確認。
3. **最終版**: 「直前に**発音した**時刻」ではなく「直前に**受け取った予約
   時刻**」(発火有無を問わない)との差分に設計変更。`rearm()` の
   「拍0を今すぐ」予約は既存の(未来の)予約より必ず小さくなるため、
   `target_ms > 直前の予約時刻` の比較だけで構造的に無視され、直後に
   続けて予約される「拍1」(新テンポでの本当の次拍)が正しい周期を返す。
   閾値のような経験則ではなく比較演算だけで成立する設計になり、ロング
   プレスを含む実機再テストで暴走が再現しないことを確認した。
   （Host API のシグネチャ自体は不変。`Midi_NotifyBeatScheduled` の
   第2引数(last_fired_ms)を削除しただけで、hostapi_defs.h の公開
   契約には影響しない。）

**検証**:
- Linux ホスト: `midibox_host` を単発実行し、同一マシンの UM-ONE へ ALSA
  シーケンサ経由で実送信、`aseqdump` で受信内容をタイムスタンプ付きで
  記録。テンポ変更前後で 24ppqn クロック間隔が約20.5ms(≈120bpm)→
  約18.5ms(≈135bpm)へ追従することを確認。ロングプレス長押しでの暴走
  再現テストも実施し、修正後は Start 1回→Clock 連続→Stop 1回のクリーン
  な系列(暴走なし、min/avg/max が現実的な範囲)を確認。
- ESP32 実機: ビルド・フラッシュ・モニタは `docker run --rm`(Phase 8a の
  教訓どおり UID 統一)で実施。ユーザーが実機タッチで
  START→BPM+ 長押し連打→STOP を操作し、UM-ONE + `aseqdump`(Linux ホスト)
  で受信内容を確認。修正後は暴走なし(min 9ms/avg 12.5ms/max 33ms、
  いずれも現実的な BPM レンジ)。`MIDI: MIDI OUT ready` ログを含め
  クラッシュなくランチャーメニューまで到達することも確認。
- `app_tick` 実行時間への影響: クロック生成は host 内部のタイマ駆動
  (esp_timer/SDL_AddTimer)で完結し、`app_tick` 側からは Start/Stop の
  1バイト送信のみ(ボタン押下時に1回)。新規の毎 tick 呼び出しは追加
  していないため、Phase 7C 時点の app_tick 負荷を悪化させる変更はない
  (定量的な再計測は今回は実施せず、設計上の非依存性で担保)。

**トラブルと教訓**:
- herdr の Linux ホスト側 SDL アプリ(`midibox_host`)は Escape キーを
  ペインへの `send-keys` では止められない(ターミナルではなく SDL
  ウィンドウがフォーカスを持つため)。プロセスを終了させるには
  `herdr pane send-keys <pane_id> "C-c"` でそのペインの前面プロセスへ
  SIGINT を送る必要がある(ESP32 monitor の docker コンテナと同じ対処)。
  これを怠ると、同じペインへの後続コマンドがすべてタイムアウトする
  (実際に `cmake --build` が 300 秒待ってタイムアウトした)。
- `aseqdump` 越しの検証は herdr pane の scrollback 上限(実測 ~998 行)に
  律速される。24ppqn クロックは 1 拍あたり 24 件出るため、数十秒の
  テストで簡単にバッファを溢れさせ、`Start` イベントが読み出し時点で
  スクロールアウトすることがある(今回複数回発生)。`Start`/`Stop` の
  対応関係を厳密に検証したい場合は、テスト時間を数秒程度に絞るか、
  ログをファイルにリダイレクトして保持する方が確実。

**検証エビデンス**: `/tmp/aseqdump_*.log`(タイムスタンプ付き、作業用
一時ファイル、リポジトリ非対象)。実機シリアルログは herdr
`esp32-monitor` ペイン scrollback。

