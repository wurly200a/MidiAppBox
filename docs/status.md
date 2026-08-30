# 現在地

各エントリの詳細は `docs/results/` の該当ファイルを参照。このファイルは
CLAUDE.md から独立して更新する(CLAUDE.md 本体は書き換えない)。

## 2026-08-23 時点

- check-workflow(docs/prompts/check-workflow.md)完了。herdr ペイン運用を
  「ラベルごとに別タブ」から「**共有タブ1つに3列×2行で分割配置**」に変更
  (`scripts/hpane.sh` 改修。`run`/`send`/`waitfor`/`read` のインタフェースは不変)。
  撮影スクリプトを `scripts/cam-rec.sh` 等に整備し、出力を `captures/`
  (.gitignore 対象)に集約。Linux ホストの画面キャプチャ(x11grab)・
  クリックによる UI 自動操作は本開発環境(Wayland/XWayland + GNOME)の制約で
  信頼できる形にできず、スコープ外として持ち越し(詳細は
  docs/prompts/check-workflow.md 追記節、docs/results/check-workflow.md)。
- Phase 7(7A 予約発音 / 7B メトロノーム / 7B-fix DMA 二重クリック / 7C トーンパレット /
  7D テンポ1刻み・ボリューム調整)完了。詳細は docs/results/phase07.md。
- Zenn 連載: 第 1〜7 回公開済み、第 8〜17 回はスケジュール公開設定済み(〜2026-07-28、詳細は docs/zenn.md)。
- Phase 8a(docs/prompts/phase08a_midi_out_bringup.md、MIDI OUT 疎通確認)完了。
  自作 MIDI OUT 回路(GPIO18=UART1 TX、2SC1815)を UM-ONE 経由で確認、
  UM-ONE LED 点灯・`aseqdump` で Note On/Off 正常受信を確認。検証専用コードは
  確認後に削除済み(Host API/ABI 変更なし)。詳細・トラブルは
  docs/results/phase08a.md。
- Phase 8b(docs/prompts/phase08b_midi_clock_api.md、MIDI Clock 出力 Host API)
  完了。`hostapi_midi_send` を追加し、メトロノームの START/STOP に MIDI
  Start/Stop を相乗り、host 内部で 24ppqn クロックを生成(実機 UART1・Linux
  は ALSA シーケンサ経由で UM-ONE へ実送信)。テンポ変更時にクロックが暴走する
  不具合を実機検証で発見し、テンポ導出ロジックを「直前発音時刻」基準から
  「直前に受け取った予約時刻」基準に設計変更して解消。詳細は
  docs/results/phase08b.md。
- Phase 8c(docs/prompts/phase08c.md、MIDI IN ハードウェア検証・受信バイト
  ダンプ)完了。TLP2361 受信回路の UART1 RX(GPIO15)を実機検証、外部機器
  (UM-ONE)からの Note On/Off・ランニングステータス・アクティブセンシング・
  SysEx(302バイト)を完全一致で受信、自機 OUT→IN ループバックでも Start/
  Clock(24ppqn)/Stop がバイト落ちなく往復することを確認(IN側直列抵抗は
  220Ωのまま変更不要)。**本 Phase の実装コード(RX 受信ダンプ機能)は
  検証専用のため `feature/midi-in-rx-dump` ブランチにのみ保持し、main には
  マージしていない。** 詳細・トラブル(フローティング入力のノイズ拾い、
  IN回路の接触不良など)は docs/results/phase08c.md。
- Phase 9a(docs/prompts/phase09a.md、MIDI IN 受信 Host API)完了。
  `hostapi_midi_recv(buf_ptr, buf_len) -> n` を追加(16 バイトアラインドの
  `hostapi_midi_recv_t { timestamp_us: u64, byte: u8, _reserved[7] }`、
  `hostapi_poll_event` と同型の out-buffer API)。実機は UART1 RX
  (GPIO15、Phase 8c 検証済み設定)のイベントタスクが受信直後に
  `esp_timer_get_time()` で打刻しリングバッファ(256件)へ積む。Linux は
  ALSA シーケンサ(snd_seq DUPLEX)経由で UM-ONE から実受信(当初案の
  「0件スタブ」からユーザー承認で実受信に変更)。パースは一切行わない
  (Phase 9b の責務)。実機・Linux 双方で UM-ONE からの Note On/Off・
  Clock の実受信とタイムスタンプ単調増加を確認、回帰(free heap 一致・
  WARN/ERROR なし)も確認済み。詳細は docs/results/phase09a.md。
- Phase 9b(docs/prompts/phase09b.md、ループバック診断アプリ)完了。
  `wasm-apps/midi_loopback/` を Stage 1(受信生バイトの16進表示)→
  Stage 2(直近24クロック移動平均からの実測 BPM)→ Stage 3(セッション統計:
  クロック間隔 min/max/σ・公称値との偏差・受信数 vs 期待数、整数 Welford 法)
  の順に実機検証しながら実装。Host API/ABI 変更なし。120bpm ループバック
  実測により、**受信クロック数が期待値を大きく下回る(試験により71〜93%
  程度)現象と、公称間隔の数十〜数百倍に達する巨大な外れ値**を複数回
  (画面無操作の条件でも)確認し、3仮説のうち「(b) クロックの取りこぼし」が
  最も有力と判断(系統的な平均間隔のずれを示す (a) の証拠はなし)。
  **追記(2026-08-17)**: ユーザーが実機配線の接触不良を発見・修正し
  再測定した結果、平均偏差は +183µs、外れ値は公称の約2倍(1回分の
  取りこぼし相当)、clocks/exp は 98.6% まで改善。**当初の大きな外れ値・
  大幅な取りこぼしの主因は配線の接触不良だった**と判断を訂正。修正後も
  残る小さな偏差(BPM 118.22、~1.4%の取りこぼし)の再現性確認が次の
  調査対象。詳細は docs/results/phase09b.md。
- herdr ペイン運用を「共有タブ1つに3列×2行」から「**セッション自身のタブに
  プロンプトを最上段・全幅(既定で高さ35%)、その下2列×3行(左列:
  esp32-build/esp32-monitor/camera、右列: unix-build/zenn/screen)**」に変更
  (`scripts/hpane.sh` 改修。ルートペインの作成元が「新規タブ」から「呼び出し元
  セッションのプロンプトペイン」に変わった以外、`ensure`/`run`/`send`/`waitfor`/
  `read` のインタフェースは不変)。`ensure-all`/`close`/`close-all` コマンドも追加。
  詳細は docs/workflow.md §6.1。
- check-workflow-routine(docs/prompts/check-workflow-routine.md)完了
  (2026-08-23)。上記の新レイアウトで §3.0〜§3.3 を一巡実行し全完走(exit 0 /
  `app_main` 到達 / free heap 開始時と一致 / 既知の1件を除き警告なし)。
  途中で `hosts/linux/build/` が旧リポジトリパスの stale な CMakeCache を
  指していてビルド失敗する事象を発見、対処を docs/workflow.md §3 に追記して
  から再実行し解消。詳細は docs/results/check-workflow-routine.md。
- CLAUDE.md とその周辺ドキュメントを整理(2026-08-23)。`docs/dev-log.md` を
  `docs/results/`(フェーズ毎ファイル、`docs/prompts/` と対応)へ分割・移動。
  「現在地」(本ファイル)・「アーキテクチャ方針」(docs/architecture.md)・
  「教訓チェックリスト」(docs/lessons.md)を CLAUDE.md から分離。herdr/hpane
  関連の重複記載は docs/workflow.md に一本化。**追記**: 当初 `docs/poc-results.md`
  はそのまま `docs/results/poc-results.md` へ移動しただけだったが、Phase 4
  専用の内容なので `docs/results/phase04.md` へ統合(`## 計測結果詳細` 節)し、
  `poc-results.md` は削除。合わせて元の `docs/results/phase01-04.md` は
  `phase01-03.md`(Phase 1〜3)と `phase04.md`(Phase 4)に分割した。
- Phase 9c(docs/prompts/phase09c.md、MIDI Clock 送信タイミングの再測定と
  欠落要因の特定)完了(2026-08-23)。`wasm-apps/midi_loopback/` に E1
  (ヒストグラム・ロバスト統計・外れ値・見かけBPM分布)を恒久機能として追加、
  実機で E1(受信側)・E2(送信側、`#ifdef PHASE9C_TXLOG_TEST` 検証専用)を
  同一セッションで3回測定。`Midi_NotifyBeatFired()` の毎拍位相リセット
  (`esp_timer_stop`→`start_periodic`)が120bpmでマージン8µsしかなく、
  クリックスケジューラのジッタで約61%の拍でクロックが1発欠落することを
  送受信双方のデータで確認(「有力仮説」節の全予測値と実測が高精度で一致)。
  E3(`#ifdef PHASE9C_FREERUN_TEST`、周期不変なら再アームしない対照実験)で
  外れ値が3回とも完全に0件になることを確認し、位相リセットが直接の原因と
  実証。E3は暫定検証のみで、検証専用コード(TXLOG/FREERUN/ログ転送フック)は
  全て削除し main の挙動を測定前に復元(`git diff` 差分なしを確認)。
  実機検証中に E2 用の検証専用バッファ(12KB)が ESP32 の一般ヒープを
  圧迫し WASM の "allocate linear memory failed" を誘発する事象を発見・
  解消(教訓を docs/lessons.md に追記)。既存アプリ7種の回帰・Linux ホスト
  起動も確認済み。詳細は docs/results/phase09c.md。
- Phase 10(docs/prompts/phase10.md、新アーキテクチャ先行調査・実装なし)開始
  (2026-08-30)。**P10-1(I2S 再生位置取得の go/no-go ゲート)完了、判定 go**。
  I2S TX の `on_sent` コールバックは無音時もフリーランで発火し(960B=240
  フレーム=5442.2µs 粒度)、打刻間隔 σ0.6〜1.0µs、線形補間誤差は負荷込み
  最悪 ±62µs(基準 ±500µs の 1/8)。既存 click/MP3 経路への干渉なしを
  A/B/A 対照で確認。調査コードは削除済み(パッチは captures/phase10/ に保存、
  P10-2 で再利用)。実機には計測ビルドが焼かれたまま(P10-2 で継続使用)。
  詳細は docs/results/phase10.md。
  **P10-2(実効サンプルレート ppm 計測)完了**: 約 7 分 × 2 回のアイドル計測で
  実効 fs = 44100.0000 Hz、公称比 −0.00 ppm、30 秒窓の変動幅 ≤0.06 ppm。
  I2S と esp_timer が同一 XTAL 系で、分数分周が 44.1kHz の厳密比
  (160MHz × 441/6250 = 256×44100)を達成するため原理的にもゼロ。
  Clock Authority の対応更新は**固定比で足りる**(逐次推定不要)と結論。
  **P10-3(ワンショット再アーム方式のジッタ実測)完了**: 絶対時刻グリッド
  (予定時刻+20833µs)のワンショット連鎖で、アイドル×2・loopback E1×2・
  mp3+タッチ×2 の全条件で**クロック欠落 0・追いつき 0**、TX 発火偏差
  max 104µs、RX 間隔 mean 20833.0µs ちょうど(σ: アイドル 21µs、最悪負荷窓
  ≤114µs)。09c の外れ値 155〜185 件 / BPM 二峰性は完全に消失。設計候補 2
  (ワンショット 1 本の L0 ディスパッチャ)の成立を確認。実機は P10-3
  計測ビルドのまま(metronome の MIDI Clock は出ない状態。次の計測で上書き)。
- 次の候補: Phase 9c で確定した原因(毎拍位相リセット)を踏まえ、ホスト側に
  音楽時間軸(テンポマップ・拍/小節カウンタ)を持たせるアーキテクチャ刷新
  フェーズの設計。120bpm 以外のテンポでの系統誤差確認、Song Position
  Pointer 等の高度な MIDI 同期はスコープ外として持ち越し。着手はユーザー
  指示待ち。
