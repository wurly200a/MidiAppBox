# 現在地

各エントリの詳細は `docs/results/` の該当ファイルを参照。このファイルは
CLAUDE.md から独立して更新する(CLAUDE.md 本体は書き換えない)。

## 2026-08-23 時点

- メニューのスクリーンセーバー / バックライト消灯(phase 外作業、2026-09-06)。
  メニュー表示中に限り、無操作 60 秒で黒地+漂う図形、180 秒でバックライト消灯。
  タッチ(または電源キー短押し)で復帰し、**復帰のタップはオーバーレイが食う**ので
  アプリを誤起動しない。アプリ実行中は無効。時間は Kconfig で変更可。
  詳細は docs/results/screensaver.md。

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
- Phase 12(docs/prompts/phase12.md、基盤整備)完了(2026-09-06)。詳細は
  docs/results/phase12.md。
  - **作業 1 パーティション拡張**: 16MB フラッシュ + `src/partitions.csv`(custom)。
    nvs / phy_init / factory のオフセットは既定表と同一のまま factory を 1MB → 4MB に拡張。
    残容量 4,112 B(0%)→ 3,149,840 B(75%)。NVS 消失なし、`erase-flash` 不要。
    起動警告 `spi_flash: Detected size(16384k) larger than ...` が消えた。
    予約データ領域は「切らない」を採用(パーティションエントリ 1 個 = internal heap 56B の実測に基づく)。
  - **作業 2 アプリ整理**: Host API カバレッジ表に基づき 10 本 → **6 本**
    (touch_demo / mp3player / clicktest / metronome / midi_loopback / seq_smoke)。
    削除は hello / demo / bars / bench(タグ `pre-app-prune`)。hello・bench 専用の
    native ハーネス(呼び出し元ゼロ)も削除。フラッシュ削減は 2,512 B で、
    **容量逼迫の主因は埋め込みではなく Flash Code(634KB)だった**ことを実測で確認。
  - **作業 3 実機テストの自動化**: USB Serial/JTAG のコマンドコンソール
    (`CONFIG_MIDIBOX_SERIAL_CMD`、既定 y。ping / ls / run / stop / heap、応答はタグ `MBCMD`)と
    `scripts/device-regress.sh` を新設。**物理操作なしで 6 本の回帰表が約 100 秒で出る**。
    以後の回帰はこれを既定とする(docs/workflow.md §2.2 / §3.4)。
    待ちはペイン出力ではなくログファイルの差分行に対して行う(スクロールバック誤マッチ対策)。
    副産物として **mp3player の「既知の −44B」は MP3 再生時にだけ出る**ことを特定した。
  - **作業 4 PSRAM 可否**: 判定 **条件付き go**。P10-4 のリブートループの真因は
    **SDMMC プローブ**(ピン競合でも DMA バッファ配置でもない)。飛ばせば PSRAM 有効で
    20/20 連続起動。ただし **PSRAM を有効にしても largest free block は 31,744 のまま増えず**、
    WASM linear memory の逼迫は緩和されない。PSRAM レイテンシ実測は internal 比 +3%
    (キャッシュ内)/ 約 2.5 倍(キャッシュ超え)。本番反映は別フェーズ。
    **main は no-PSRAM 構成**で最終自動回帰に合格(free heap 49160 / largest 31744 / 警告 0)。
  - **重要な運用上の発見**: `allocate linear memory failed` が出たら SD の初期化経路を疑う。
    SDMMC(20.00 MHz)なら largest 31744 で正常、SDSPI フォールバック(11.43 MHz)だと
    15360 に落ちて全アプリが起動しない。**復旧は USB 抜き差しによる電源断**
    (ソフトリセットでは直らない)。P10-4 の「実際に使われるのは SPI 経路のみ」という
    記述はこの実測で訂正した。

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
  **P10-4(メモリ監査)部分完了**: 4 時点のヒープ実測(WASM アプリ実行中の
  largest free block は 13〜14KB = 9c の逼迫水準と一致)、internal 16B ランダム
  アクセス ~360ns/op(mp3 再生中に最大 1385ns/op へ跳ねる回あり)、L0 キューは
  **internal 静的 BSS 4KB = 256 イベント**(4 声部 16 分音符で 2 小節分)が
  現実的な出発点と結論。**PSRAM(8MB octal 搭載)は有効化すると SD 初期化の
  SDMMC プローブで TG1WDT リブートループに陥ることが判明**し、選択肢から除外
  (教訓を docs/lessons.md に追記)。PSRAM 側レイテンシベンチのみ未取得。
  実機は main ビルドへ復旧済み(正常起動を確認)。
  **P10-5(UART リアルタイムバイト割り込み挿入 PoC)完了**: 0xF8 を挟んでも
  ノート列は 24,043 メッセージでエラー 0(完全一致)。TX FIFO 占有量プローブに
  より **`uart_write_bytes` で足り、FIFO 直叩きは不要**(挿入遅延は平均 16µs・
  最悪 640µs)と確定。ノート送出時に見えた σ1.19ms は、ペーシング実装
  (FIFO 占有が証明可能にゼロ)でも同値だったことから**受信側の打刻バッチング
  由来の計測アーティファクト**と切り分け済み(`rx_task` が UART イベント内の
  3〜4 バイトに同一時刻を付けるため。ボーレート既知なので
  `T-(N-1-i)x320µs` で補正可能)。SL MK3 実機でもノート受信中のクロック検知
  テンポが安定(ユーザー目視確認)。ただし SysEx 等の大バーストを一括で
  FIFO に流すと最大 41ms 待たされるため、ポート層は分割送出すること。
- **Phase 10 の調査項目 P10-1〜P10-5 は完了**(P10-4 の PSRAM ベンチのみ
  別途実施)。実機は main ビルドで正常動作中(調査コードは全て削除済み)。
- **Phase 10 の設計書一式を作成し、現在ユーザーレビュー中(未承認)**
  (2026-08-31)。成果物は 2 ファイル:
  - `docs/architecture-next.md`(**後に `docs/architecture.md` へ確定反映し削除**):
    層構成 L0〜L3、Clock Authority(レートマスター = I2S サンプルカウント)、
    tick 座標系(PPQN 960 / u32)、MIDI クロックのグリッド生成、ポート抽象と
    送出規律、メモリ配置方針、Phase 11 以降の移行順序案、**数値根拠表**
    (全定数を P10-1〜5 の実測値に紐付け)。
  - `docs/hostapi-next.md`: Host API 仕様案(`transport_*` / `tempomap_*` /
    `seq_*` / `time_us_to_tick` の全 12 関数)。ABI レイアウト・エラーコード・
    既存 API との関係(残す/非推奨化する)、**アプリ要件突き合わせ表**、
    `shared/hostapi_defs.h` へ取り込むコード片。
  - 検証結果: **5 要件(高精度メトロノーム / 楽曲メトロノーム / SMF インポート /
    2trk シーケンサー+録音 / ドラムマシン)をすべて通しても API 語彙は
    増えなかった**(指示書のレビュー観点を満たす)。
  - **既存の `docs/architecture.md` と `shared/hostapi_defs.h` は未変更**
    (承認前に実装フェーズの作業を始めないゲートを守るため、改訂案は
    別ファイルとして提示している)。
  - レビュー論点は §11 に 5 点を列挙した。特に
    論点 1(ループの tick 表現: L0 のソートキーを単調増加に保つため
    playback tick / song tick の 2 座標に分離した案)。
  - **初版レビュー完了(2026-09-05)**。承認条件とされた 3 点
    (ループ tick 表現のトレードオフ明記 / L0 ディスパッチャのロック規律 /
    レート切替時の Clock Authority 継続規則)を反映済み。論点 2〜5 と
    追加論点(STOPPED 中のクロック挙動)の決定も `docs/architecture.md` §11
    に記録した。
- **Phase 10 の最終回帰完了(2026-09-05)、合格**。
  - **実機 7 アプリ**: free heap は全アプリで開始時と一致(mp3player のみ
    −44B = 9c 以前からの既知挙動)。largest block は全区間 31744 で不変。
  - **P10-3 で観測した midi_loopback の −220B は計測ビルド起因と確定**
    (main では +0B)。**main に恒久的なリークはない**。
  - metronome 実行中に `MIDI RX: ring buffer full` が 239 件出たが、
    **ループバック配線を挿したまま、受信をドレインしないアプリを動かした
    ことによる構成依存の挙動**と特定(256 件 ÷ 24ppqn = 5.33 秒後に初回警告、
    実測 5.3 秒と一致)。main の元コードのままで Phase 10 の変更とは無関係、
    通常使用では発生しない。新アーキテクチャでは L0 が常時ドレインするため
    Phase 11 以降で自然に解消する見込み。
  - **Linux ホスト 7 アプリ**: 全て `app_init=0` / `app started` / `app stopped`、
    警告・エラー 0 行、残留プロセスなし。
  - 詳細は `docs/results/phase10.md` の「最終回帰」節。
- **Phase 10 完了(2026-09-05)。設計を確定した。**
  - 確定反映は 2 段に分けた(ユーザー承認済み):
    1. **実施済み**: `docs/architecture-next.md` の内容を `docs/architecture.md`
       へ反映し、ドラフトは削除した。ドラフト表記を外し、§11 を
       「設計判断の記録」として残してある(**§11-1 の「意図的に放棄した性質」は
       削除しないこと**)。
    2. **Phase 11 のステップ 2 で実施**: `docs/hostapi-next.md` §8 のコード片を
       `shared/hostapi_defs.h` へ取り込む。Phase 10 のゲート「本フェーズでは
       Host API / ABI を変更しない」に抵触するため本フェーズでは行わない。
  - `docs/hostapi-next.md` は**承認済みの仕様**として残す(Phase 11 で
    hostapi_defs.h へ反映したのち、置き場所を整理する)。
- **次のフェーズ(Phase 11)の推奨スコープ**(レビューでの助言):
  - 移行表(`docs/architecture.md` §10)の**ステップ 1〜3 まで**を 1 フェーズとする。
    ステップ 3(metronome を新 API で書き直し → midi_loopback の E1 統計で
    9c と前後比較)が本改訂の価値を初めて実証する地点で、ここまでで
    「クロック欠落 61% → 0」という対外的に語れる結果が出る。
  - ステップ 4〜5(既存経路の置換・`hostapi_midi_send` の副作用削除)は
    挙動変更を含むので**別フェーズに分ける**(回帰の切り分けが楽になる)。
  - **Linux ホストへの同時実装(ステップ 2)は必ず同フェーズ内で行う**。
    遅らせると Clock Authority の抽象が実機都合に引きずられ、Phase A
    (ブラウザ)の移植性という当初の狙いが検証されないまま固まる。
    Linux 側のレートマスターは SDL オーディオコールバックの累計サンプル数で、
    `on_sent` と同型に書けるはず。
- **別途切り出した独立課題**: PSRAM 有効化時に SDMMC プローブがハングする件
  (P10-4)。将来サンプルプレーヤーの波形メモリで PSRAM が必要になった時のため。
- 次の候補: Phase 9c で確定した原因(毎拍位相リセット)を踏まえ、ホスト側に
  音楽時間軸(テンポマップ・拍/小節カウンタ)を持たせるアーキテクチャ刷新
  フェーズの設計。120bpm 以外のテンポでの系統誤差確認、Song Position
  Pointer 等の高度な MIDI 同期はスコープ外として持ち越し。着手はユーザー
  指示待ち。

## 2026-09-06 時点

- **Phase 11(docs/prompts/phase11.md、新アーキテクチャの実装)完了。**
  詳細は `docs/results/phase11.md`。
  - **ステップ 0(設計の穴埋め、承認済み)**: `seq_write` の部分受理を
    **プレフィックス受理 + アプリが残りを保持する契約**で確定
    (`architecture.md` §11-9)。代案「全件受理か 0 か」は、チャンクが空きを
    上回ると**前進しないまま無音になる**うえ「空き件数の照会」という語彙を
    増やす方向に働くため不採用。あわせて、キューの未発火イベントを破棄する
    操作(`transport_locate` / `seq_flush_after` / `transport_stop`)の後は
    アプリ側の未受理分も破棄して `seq_filled_until()` から供給し直す契約を明記。
    未発火 note-off の破棄(鳴りっぱなし)は **v1 はアプリ責務のまま凍結**
    (§11-8。`hostapi_midi_send` が L0 を通らない以上ホスト側の追跡は原理的に
    不完全になるため)。
  - **ステップ 1(L0/L1 を native に実装、既存経路と並存)完了**。静的追加は
    約 4.6KB(L0 キュー 256 件 = 4KB + テンポ/拍子マップ)。Clock Authority は
    I2S TX の `on_sent` をレートマスターにし、固定比換算・アンカー・レート切替時の
    継続規則・ppm 監視を 1 モジュールに閉じ込めた。
  - **ステップ 2(Host API 12 関数、実機 + Linux 同時)完了**。実装の重複を避けるため
    **L0/L1 のロジックを `shared/seq_core.c`(移植可能な C)へ切り出し、両ホストが
    同一ソースを使う**形にした。プラットフォーム依存は 7 個のフック
    (`now_us` / `lock` / `unlock` / `arm` / `disarm` / `send_midi` / `click`)に
    外出ししてあり、ここが Phase A(ブラウザ)への移植点になる。
    Linux のレートマスターは SDL オーディオコールバックの累計フレーム数、
    時刻源は `hostapi_midi_recv` と同一時基の単調増加 µs、ディスパッチは
    `pthread_cond_timedwait`(CLOCK_MONOTONIC。`SDL_AddTimer` は ms 分解能で
    20833µs のグリッドを表現できない)。
  - **12 関数すべてを実機・Linux 双方で検証済み**。`wasm-apps/seq_smoke/` を
    自動一巡する検証アプリに拡張し、**同一の .wasm** で実機 `chk 255` /
    Linux `chk 255`(8 項目すべて合格)。ALSA 経由で採った送出タイミングは
    120bpm: mean **20833.1µs** / σ33.6、180bpm: mean **13888.9µs** / σ32.3 で、
    PLAYING 中のテンポ変更がキュー積み直しなしに効くことも確認。
  - **回帰**: 実機・Linux とも既存 7 アプリに影響なし。実機の `largest block` は
    Phase 10 最終回帰と同じ **31744** のまま(本フェーズを通して一度も縮んでいない)。
  - **ドキュメント整理**: `docs/hostapi-next.md` → **`docs/hostapi.md`** に改名し、
    §8 のコード片は `shared/hostapi_defs.h` への参照に置き換え(二重管理の解消)。
    §3 の `transport_locate` の記述矛盾(「次の start/continue の開始位置」)を修正。
- **スコープ変更(2026-09-06、指示書の追記節)**: ステップ 3(metronome の書き直しと
  前後比較)は **Phase 12 へ移管**。理由は、実機が WASM アプリを 1 つしか動かせず
  「metronome を動かしながら midi_loopback の E1 で測る」が成立しないこと、
  Phase 09 の実装は実用に耐えず「前」の再測定に価値がないこと、アプリパーティション
  残が少ないこと。Phase 12 は**絶対値目標**(欠落 0 / clocks÷expected = 100% /
  BPM 単峰 / 平均間隔 20833µs)で判定し、metronome は別ディレクトリを作らず
  **上書きで書き直す**。
- **申し送り**: アプリパーティション残が 4112B(0%)。実フラッシュは 16MB あるが
  設定が 2MB。Phase 12 で足りなくなったら seq_smoke の埋め込みを外すか設定を
  見直す。また現在 Linux で ALSA が使えない(リモートデスクトップ経由)ため、
  タイミング測定の前に ALSA が使える状態を用意する必要がある。

- **Phase 13(docs/prompts/phase13.md、metronome を新 API で書き直す = 移行ステップ 3)
  完了(2026-09-06)。** 詳細は `docs/results/phase13.md`。
  - `wasm-apps/metronome/` を**音楽時間軸 API(transport / tempomap / seq)だけ**で
    上書き書き直し。旧経路(`hostapi_click_schedule` / `hostapi_tone_schedule` /
    `hostapi_midi_send`)は `extern` から外し、`.wasm` の import にも現れない。
    クリックは `seq_write(port=CLICK / OP_TONE)` で playback tick に予約、
    MIDI Clock は L1 がグリッドから生成する。`.wasm` は 2,885 → 3,910 B。
  - **演奏中のテンポ/拍子変更は「`transport_locate(0)` → `at_tick=0` のエントリを上書き」**
    の即時方式にした(指示書の「次の小節頭に積む」案から変更、承認済み)。理由は
    (a) 異なる at_tick へ積むとテンポマップ(上限 32)が枯渇する、
    (b) 長押し連打が 1 小節に 1 回しか効かず旧版の機能を維持できない、の 2 点。
    旧版の `rearm(now)`(変更した瞬間から小節をやり直す)と同じ意味論になる。
  - **実測(実機 MIDI OUT → UM-ONE → PC、120bpm・4/4、アイドル 5.5 分 ×3)**:
    **クロック欠落 0 件 / clocks÷expected 100.00% / 見かけ BPM 単峰 /
    平均間隔 20832.8µs**。**09c の「約 61% の拍で 1 発欠落」「BPM 二峰性」は消失**。
    3 回中 1 回だけ外れ値 2 件が出たが、「50ms の空白 → 3µs で 2 発」という並びで、
    DIN の 1 バイト時間(320µs)より短い間隔は物理的にありえないため受信側
    (USB/ALSA)の配送アーティファクトと判断(クロック総数は期待値と完全一致)。
  - 負荷条件(長押しでテンポを 120→237→120 と動かし続けた直後の静粛区間 104.5 秒)でも
    **欠落 0 / 100.0% / 平均 20832.9µs**。
  - **測定ツールを新設**: `tools/midi_clock_probe/`(ALSA のカーネル打刻で受信を記録する
    C プローブ + Python 集計)と `scripts/midi-clock-probe.sh`。使い方は
    `docs/workflow.md` §3.5。seq_smoke で妥当性を確認済み(Phase 11 の実測値と一致)。
  - **回帰 PASS**(6 本、free heap 差分 +0、largest block 31744、警告 0)。
    clicktest は旧経路のまま動作。free heap の水準は 49160 → 49136(−24B、全アプリ同値)。
  - **外部機器での確認(SL MK3)**: 検知テンポは安定し一度も外れず、表示値と期待値が一致。
    演奏中に切り替えても追従し、**BPM 下限 40 / 上限 240 の両端も OK**(ユーザー目視)。
  - **スコープ変更(ユーザー判断)**: 条件 B の正規実行・条件 C(演奏中テンポ変更)・
    条件 D(送信側 σ)はスキップ。条件 D 用の検証コードは実装したが測定しないので削除済み。
  - 次の候補は移行ステップ 4 / 4b / 5(旧クリック経路の置換・削除、
    `hostapi_midi_send` の Start/Stop 副作用の削除)。旧経路の残る利用者は
    clicktest と midi_loopback のみ。

- **Phase 14(docs/prompts/phase14.md、旧経路の削除 = 移行ステップ 4b / 5)
  完了(2026-09-06)。** 詳細は `docs/results/phase14.md`。
  - **ステップ 1**: `wasm-apps/midi_loopback/` を音楽時間軸 API へ移行。
    `hostapi_transport_start/stop` + `tempomap_set_tempo/meter`(120bpm 固定)に
    一本化し、`hostapi_click_schedule` / `hostapi_midi_send` を `extern` から
    削除。可聴クリックは供給しない判断(受信統計に条件を絞る)。E1 統計は
    恒久機能のまま維持。実機ループバック測定(自機 MIDI OUT → 自機 MIDI IN、
    120bpm・4/4、約 6.3 分)で**外れ値 0 件 / clocks÷expected 100.00%
    (18255/18255)/ 見かけ BPM 単峰 / 平均間隔 20836µs**(目標 20833±10µs)を
    確認。測定には Phase 9c と同じ手法(画面外センチネル座標 + 検証専用
    シリアルログ転送フック `PHASE14_STATLOG_TEST`)を使い、測定後に削除。
  - **ステップ 2**: `wasm-apps/clicktest/` を削除(回帰対象 6 本 → 5 本)。
    カバレッジの穴が空かないことを確認済み(`docs/results/phase12.md` 追記)。
  - **ステップ 3**: `hostapi_click_schedule` / `hostapi_tone_schedule` を
    `shared/hostapi_defs.h`・両ホストから削除(native 実装・予約状態・
    `Midi_NotifyBeatScheduled/Fired` の呼び出し側を含む)。トーンパレットの
    即時発音(`tone_define` / `tone_play` / `play_click`)は無影響。
  - **ステップ 4**: `hostapi_midi_send` の Start/Stop 副作用と旧クロック
    生成器(`s_clock_running` 等のテンポ逆算状態、`Midi_NotifyBeatScheduled/
    Fired` の定義)を両ホストから削除。**9c の根本原因(テンポの二重管理・
    毎拍位相リセット)がコードから物理的に消えた**。
  - **回帰 PASS**(5 本、free heap 差分 +0、largest block 31744 で不変、
    警告 0)。free heap の水準は削除が進むごとに増加(49136 → 49288、
    esp_timer ハンドル 2 個分の解放)。Linux ホストも 5 本が
    `app_init=0` / `app started` / `app stopped`、警告 0 で起動・終了。
  - フラッシュ使用量: Phase 13 時点 1,048,816 B → Phase 14 完了時
    1,045,984 B(**−2,832 B**)。
  - 検証専用コード(`PHASE14_STATLOG_TEST`)は削除済み、
    `git grep PHASE14 -- src scripts wasm-apps tools` は該当なし。
  - **次の候補**: 移行ステップ 6(内蔵音源のポート追加)。PSRAM の本番反映は
    Phase 12 の「条件付き go」のまま別フェーズ。`hostapi_midi_recv` の
    タイムスタンプ線速補正(docs/hostapi.md §7)も未実施のまま持ち越し。

- **Phase 15(docs/prompts/phase15.md、PSRAM 本番反映 = WASM linear memory の
  PSRAM 移行)進行中・中断(2026-09-06)。** 詳細は `docs/results/phase15.md`、
  設計は `docs/design/phase15-psram.md`。
  - **Phase 12 の「PSRAM を有効にしても効果なし」判定は誤りだった。** largest free block が
    31,744 のまま動かなかったのは事実だが、**linear memory がその領域から出て PSRAM へ
    移っていた**からである。当時 `memory_data` のアドレスを見ていなかったため気づけなかった。
  - **採用: A 案 + `CONFIG_SPIRAM_USE_CAPS_ALLOC`**(`sdkconfig.defaults` のみの変更)。
    `CONFIG_SPIRAM=y` にすると IDF のリネーム機構が旧名 `CONFIG_ESP32S3_SPIRAM_SUPPORT` を
    立て、WAMR が `-DWASM_MEM_DUAL_BUS_MIRROR=1` を付け、`os_mmap` が
    `MALLOC_CAP_SPIRAM` を使う。**`managed_components/` の書き換えは不要。**
  - **T-3 達成**: `-zstack-size` を上げた `touch_demo` で linear memory
    **73,840 B**(internal largest 57,344 超)と **8,257,136 B**(PSRAM largest の 400B 下)が
    実機で起動。8,396,912 B は失敗。**上限は PSRAM の最大連続ブロックで決まる**(基準の約 500 倍)。
  - **`--initial-memory` を上げても効かない**(WAMR の `WASM_ENABLE_SHRUNK_MEMORY` が潰す)。
    実サイズは `align8(__heap_base) + instantiate の heap_size`。
  - 実測(アプリ実行中): `free_int` 105,880 / `largest_int` 57,344 /
    `free_psram` 8,316,904。5 アプリとも int・psram 差分 0、WARN/ERROR 0 件。
  - **LVGL 描画バッファは PSRAM へ移る**(自動)。`psram_dma_direct = 1` を立てないと
    spi_master が転送のたび internal に 19,200 B のバウンスを取る(実測 `largest_int` −20,480B)。
  - **回帰の指標を internal / PSRAM の 4 値に改訂**(`app: stopped free_int=… largest_int=…
    free_psram=… largest_psram=…`)。判定を「余裕の監視 = 下限しきい値」と
    「リーク検出 = 差分の厳密一致」に分離した(`scripts/device-regress.{sh,conf}`)。
  - **既知の穴(本フェーズで新設した監視の未完部分)**: **PSRAM のリーク監視は
    「1 回の起動→停止の差分」までで、同一アプリを N 回反復したときの非減少判定
    (4c)は未実装。** linear memory が PSRAM から取られる以上ここは実質的な監視点なので、
    次フェーズで入れること。
  - **未実施: T-1(MIDI クロック)・T-2 の `app_tick` 統計・T-4(20 回連続。1/20 で中断)・
    ステップ 3 のカメラ目視確認・ステップ 4(SDMMC と PSRAM の共存)。**
    **main は PSRAM 有効のまま残しているが、T-1 が未検証**であることに注意
    (指示書は「T-1 が不合格なら PSRAM 無効に戻す」としている)。
  - ステップ 4 への申し送り: **H1(ピン競合)は否定**(SDMMC は CLK=14/CMD=17/D0=16、
    octal PSRAM は GPIO 33〜37)。**H5(PSRAM 由来バッファが DMA 経路へ)は生きている** —
    Phase 12 は `CAPS_ALLOC` でも同じ失敗だったことを根拠に否定したが、
    **`CAPS_ALLOC` は `heap_caps_malloc(..., MALLOC_CAP_DEFAULT)` を internal に留めない**
    (`malloc()` の実体だけが `MALLOC_CAP_INTERNAL` を足して呼び直す実装)。
