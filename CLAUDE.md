# MidiAppBox — WASM PoC

ESP32-S3-Touch-LCD-2.8 (Waveshare) ベースの音楽デバイスファームウェア。
「サンドボックス化された WASM アプリを組込みデバイスに配信する音楽プラットフォーム」の
成立性検証 PoC を進行中。

## ドキュメント構成(必読)

- **CLAUDE.md(本ファイル)**: 常時従うルールと現在地のみ。
- **docs/prompts/**: フェーズ指示書(課題定義・完了条件の原本)。
  フェーズ開始時にユーザーが指定するファイルを読み、その指示に従う。
  スコープ変更は本文を書き換えず末尾に「追記 (日付)」節を足す。
- **docs/dev-log.md**: Phase 0〜 の調査・計画・実施記録・実測値・トラブルの詳細。
  過去 Phase に関わる作業(API 変更、メモリ調整、回帰など)の前に該当 Phase を読むこと。
- **docs/zenn.md**: Zenn 連載の運用ルール・連載構成対応表。
  記事の作成・更新作業を行うときは、作業前に必ず全体を読むこと。
- **docs/poc-results.md**: Phase 4 の計測結果と所見。
- **docs/workflow.md**: 標準開発ワークフローの原本(不変条件と推奨手順)。フェーズ開始時・動作確認時に参照。

## 言語

- ユーザーとのやりとり(会話・報告・質問)は日本語。
- git のコミットメッセージは英語。

## 現在地 (2026-08-17 時点)

- check-workflow(docs/prompts/check-workflow.md)完了。herdr ペイン運用を
  「ラベルごとに別タブ」から「**共有タブ1つに3列×2行で分割配置**」に変更
  (`scripts/hpane.sh` 改修。`run`/`send`/`waitfor`/`read` のインタフェースは不変)。
  撮影スクリプトを `scripts/cam-rec.sh` 等に整備し、出力を `captures/`
  (.gitignore 対象)に集約。Linux ホストの画面キャプチャ(x11grab)・
  クリックによる UI 自動操作は本開発環境(Wayland/XWayland + GNOME)の制約で
  信頼できる形にできず、スコープ外として持ち越し(詳細は
  docs/prompts/check-workflow.md 追記節、docs/dev-log.md)。
- Phase 7(7A 予約発音 / 7B メトロノーム / 7B-fix DMA 二重クリック / 7C トーンパレット /
  7D テンポ1刻み・ボリューム調整)完了。詳細は docs/dev-log.md の Phase 7 節。
- Zenn 連載: 第 1〜7 回公開済み、第 8〜17 回はスケジュール公開設定済み(〜2026-07-28、詳細は docs/zenn.md)。
- Phase 8a(docs/prompts/phase08a_midi_out_bringup.md、MIDI OUT 疎通確認)完了。
  自作 MIDI OUT 回路(GPIO18=UART1 TX、2SC1815)を UM-ONE 経由で確認、
  UM-ONE LED 点灯・`aseqdump` で Note On/Off 正常受信を確認。検証専用コードは
  確認後に削除済み(Host API/ABI 変更なし)。詳細・トラブルは docs/dev-log.md
  の Phase 8a 節。
- Phase 8b(docs/prompts/phase08b_midi_clock_api.md、MIDI Clock 出力 Host API)
  完了。`hostapi_midi_send` を追加し、メトロノームの START/STOP に MIDI
  Start/Stop を相乗り、host 内部で 24ppqn クロックを生成(実機 UART1・Linux
  は ALSA シーケンサ経由で UM-ONE へ実送信)。テンポ変更時にクロックが暴走する
  不具合を実機検証で発見し、テンポ導出ロジックを「直前発音時刻」基準から
  「直前に受け取った予約時刻」基準に設計変更して解消。詳細は docs/dev-log.md
  の Phase 8b 節。
- Phase 8c(docs/prompts/phase08c.md、MIDI IN ハードウェア検証・受信バイト
  ダンプ)完了。TLP2361 受信回路の UART1 RX(GPIO15)を実機検証、外部機器
  (UM-ONE)からの Note On/Off・ランニングステータス・アクティブセンシング・
  SysEx(302バイト)を完全一致で受信、自機 OUT→IN ループバックでも Start/
  Clock(24ppqn)/Stop がバイト落ちなく往復することを確認(IN側直列抵抗は
  220Ωのまま変更不要)。**本 Phase の実装コード(RX 受信ダンプ機能)は
  検証専用のため `feature/midi-in-rx-dump` ブランチにのみ保持し、main には
  マージしていない。** 詳細・トラブル(フローティング入力のノイズ拾い、
  IN回路の接触不良など)は docs/dev-log.md の Phase 8c 節。
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
  WARN/ERROR なし)も確認済み。詳細は docs/dev-log.md の Phase 9a 節。
- Phase 9b(docs/prompts/phase09b.md、ループバック診断アプリ)完了。
  `wasm-apps/midi_loopback/` を Stage 1(受信生バイトの16進表示)→
  Stage 2(直近24クロック移動平均からの実測 BPM)→ Stage 3(セッション統計:
  クロック間隔 min/max/σ・公称値との偏差・受信数 vs 期待数、整数 Welford 法)
  の順に実機検証しながら実装。Host API/ABI 変更なし。120bpm ループバック
  実測により、**受信クロック数が期待値を大きく下回る(試験により71〜93%
  程度)現象と、公称間隔の数十〜数百倍に達する巨大な外れ値**を複数回
  (画面無操作の条件でも)確認し、3仮説のうち「(b) クロックの取りこぼし」が
  最も有力と判断(系統的な平均間隔のずれを示す (a) の証拠はなし)。
  根本原因の特定(host 側クロック生成タイマ or WASM 側 tick 予約間隔の
  疑い)は次フェーズの調査対象として dev-log に提案済み(送信側修正は
  本フェーズのスコープ外)。詳細は docs/dev-log.md の Phase 9b 節。
- 次の候補: Phase 9b で見つかったクロック取りこぼし(取りこぼし率
  71〜93%、公称間隔の数十〜数百倍の外れ値)の原因調査。Song Position
  Pointer 等の高度な MIDI 同期はスコープ外として持ち越し。着手はユーザー
  指示待ち。

## 開発の進め方(このリポジトリでの作業ルール)

- 「小さいターゲットを定めて、テストし、次を計画する」の反復。各フェーズはビルドが通り
  コミット可能な粒度を保つ。
- 開発環境は **herdr**。シェル実行はすべて下記「シェル実行(要点)」と
  **docs/workflow.md** に従い `scripts/hpane.sh` 経由で行う。
- Phase 完了まで、ビルド・フラッシュ・モニタ確認を含めて確認なしで自律的に進めてよい。
  各ステップの結果はログとして **docs/dev-log.md** に残すこと。
  実施記録の冒頭には対応するフェーズ指示書(docs/prompts/phaseXX.md)への参照を書く。
- 実機検証は Web カメラ(/dev/video0)で実機を撮影して行う
  (`scripts/cam-rec.sh` / `cam-still.sh`。具体的な使い方は workflow.md §3.3)。
  作業確認用の一時キャプチャは `captures/`(.gitignore 対象)、Zenn 記事の素材として
  残すものは従来どおり `~/ビデオ/zenn-phaseXX/` にコピーする。
- 依存追加は最小限。追加時は本ファイル末尾「依存の記録」に理由を残す。
- 既存アプリ(demo / bars / mp3player / metronome / clicktest)の回帰を壊さない。
  (旧「MP3 デモモードで分岐」ルールは Phase 6D で解消済み。履歴は docs/dev-log.md 冒頭。)
  
## アーキテクチャ方針(決定済み)

- タイミングクリティカル層(オーディオ出力、描画ドライバ、将来の FM 音源)はネイティブ
  ホスト側。WASM アプリはロジックのみを持ち、ホスト API を叩く。
- WASM アプリは Rust / `wasm32-unknown-unknown`(WASI 不使用)。
- 実機ランタイムは WAMR。まず interpreter で動かし、AOT は後続フェーズ。

## シェル実行(要点)

シェルで実行するもの(ビルド、フラッシュ、モニタ、カメラ撮影、動作確認)はすべて、
直接 Bash ではなく `scripts/hpane.sh` 経由で herdr のペインで実行する。
**ペイン構成、コマンドの具体形、タイムアウト既定値、初回セットアップ、
セッション初回の確認手順の原本は docs/workflow.md。**
シェル実行を伴う作業の開始前に必ず読むこと。

常時従う要点(詳細と根拠は workflow.md §1・§6):

- 完了待ちは `hpane.sh run` の番兵トークン方式のみ。`herdr wait output` を
  ログ文言に直接マッチさせない。exit code が成否(124 はタイムアウト)。
  失敗・タイムアウト時は `read` でログを確認して報告し停止する。勝手に次へ進まない。
- pane ID を記憶・再利用しない(ラベルから毎回解決)。ペインに対話状態を
  持たせない(毎回一発コマンド)。新しいラベルの追加は事前にユーザー承認。
- Docker イメージタグはリポジトリトップ README.md 準拠。build と flash/monitor は
  同一タグ。devcontainer CLI は使わない。
- 推奨手順からの逸脱・改善は、実行前に提案して承認を得る(workflow.md §5)。

## 教訓チェックリスト(詳細な経緯は docs/dev-log.md の該当 Phase)

### メモリ(ESP32)
- 大きな静的バッファを足したら free heap に加え `largest_free_block` を必ず確認(5A, 6B, 7B-fix)。
- ヒープからの恒久確保(タスク等)は最大連続ブロックを分断する。恒久物は静的確保に(7B-fix)。
- WAMR プールは現在 **48KB**(実測消費 ~27.5KB)。Linux も parity で 48KB を維持(7B-fix)。
- FATFS は sector 512 + max_files 4(6B。sector 4096 は連続ヒープ ~38KB を要求し WAMR と衝突)。

### WAMR
- WASM 実行スレッドは pthread で作る(`os_self_thread()` が `pthread_self()` を呼ぶ)(P1)。
- `wasm_runtime_load` に渡したバッファは unload まで保持(fast-interp は in-place 書き換え)(P1)。
- component の Kconfig 既定は全部盛り。LIB_PTHREAD 有効のままだと
  `wasm_runtime_create_exec_env` が失敗する(P1)。
- .wasm は必ず `-zstack-size` を縮小(既定だと Rust はスタック 1MB を要求)(P0)。

### 実機運用
- SD シード後に magic 不一致が続いたら SD 側の FS 破損を疑う(手動コピー / 再フォーマット)(7A)。
- monitor 再起動は既定でボードをリセットする。`--no-reset` は `-p <port>` 指定必須(7A)。
- monitor は `PYTHONUNBUFFERED=1 ... | tee <ログ>` で落とし、ホスト側ファイルポーリングで読む(7A)。
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

### herdr / ビルド
- 完了待ちは hpane.sh の番兵方式のみ。ログ文言への `wait output` 直マッチは
  高速スクロール行を取りこぼす(6C)。
- pane の cwd ドリフトに注意。ビルドは絶対パス+成果物のタイムスタンプ/シンボル確認を
  セットで行う(7C)。
- herdr の pane_id/tab_id は `<workspace>:p<N>`/`<workspace>:t<N>` 形式(`\d+-\d+` ではない)。
  `tab list`/`tab get` は pane_id を含まないため `pane list` と `tab_id` で突き合わせる必要がある(7D)。
- `python3 - <<'PY' ... PY`(ヒアドキュメント)は stdin を script 本体で使い切り、
  パイプ入力を読めない。パイプ入力を読むワンライナーは `python3 -c` で渡す(7D)。
- ESP32 ビルド用コンテナと flash/monitor 用コンテナはイメージタグを揃える。
  同名イメージでも tag 違い(pull 時期違い)でトゥールチェーンパスが食い違い、
  他方の `build/` キャッシュに対して壊れる(7D)。flash/monitor には
  `--device=/dev/ttyACM0 --group-add <dialout gid>` を付けた `docker run --rm -it`
  が必要(`-i` のみだと `idf_monitor` が real TTY 要求で失敗)(7D)。
- herdr の pane は `herdr pane split <pane_id> --direction right|down` で分割生成でき、
  `herdr pane rename <pane_id> <label>` でペイン単位に(タブラベルとは別軸の)
  ラベルを付けられる。`herdr pane list` の各要素の `label` フィールドで直接解決できるため、
  複数ラベルを 1 タブに同居させて同時視認できる(check-workflow)。
- **`devcontainer up`/`devcontainer exec`(devcontainer CLI)が作る UID remap 済み
  イメージは ESP32 ビルドに使わないこと。** README と同じ生イメージへの
  `docker run` + `docker exec` では通る同一ソース・同一 pin バージョンの
  managed component が、devcontainer CLI 経由のビルドだと `-Wignored-qualifiers`
  が `-Werror` 化されてビルド失敗する現象を確認(根本原因未特定、check-workflow で回避)。
- `docker run ... bash -lc '...'` はログインシェル扱いで `~/.bashrc`
  (IDF の `export.sh` を source している)を読まない。非対話一発コマンドで
  `idf.py` を使うには `bash -c 'source /opt/esp-idf/export.sh && idf.py ...'`
  のように明示 source すること(check-workflow)。
- `managed_components/`(gitignore 対象)を「再取得可能なキャッシュ」と即断して
  中身を確認せず `rm -rf` してはならない。ハッシュ不一致で `idf.py fullclean` が
  保護的に停止した場合、削除前に該当ファイルの差分を確認すること
  (check-workflow で、ローカル修正が入っていた可能性のあるファイルを不用意に
  削除してしまった)。
- README タグの生イメージによる持続コンテナ(`esp32-build` 用)は自動では
  存在しない。`docker ps -a` で見当たらなければ `docker run -d --name <container>
  -v <repo>:/workspaces/MidiAppBox -w /workspaces/MidiAppBox <README タグ>
  sleep infinity` で起動してから `docker exec` する(check-workflow-routine)。
- README タグの生イメージの `entrypoint.sh` は起動時カレントディレクトリ
  (`-w` の値)の所有者に UID/GID を合わせて `gosu` で降格してからコマンドを
  実行する。`docker run`(entrypoint 経由)はこれでホスト UID になるが、
  `docker exec` はこれを経由せずイメージ既定ユーザー(root)で実行される。
  同一の持続コンテナに対して `exec`(root)と都度起動の `run --rm`(gosu
  降格後 UID)を混在させると、`build.ninja`/`.ninja_log` 等の所有者が割れて
  以後のビルドが `Permission denied` で失敗する。ビルド・フラッシュ・
  モニタは同じ実行方式(`docker run --rm`)に統一するのが安全(8a)。
- `scripts/hpane.sh send` はテキスト入力のみで割り込みキーは送れない。
  `idf.py monitor` 等を止めるには `herdr pane send-keys <pane_id> "C-c"`
  を使う(`"C-]"` のような角括弧付きキー名は `invalid_key` で拒否される)(8a)。
  ただし `C-c` が `idf_monitor` に効かず(`Writing to serial is timing out`
  を繰り返すだけで停止しない)場合がある。その際は `docker ps` で該当
  コンテナ ID を確認し `docker kill <container id>` する方が確実(8c)。
- 長時間(概ね数分)接続した `idf.py monitor` が、`docker ps` 上は `Up` の
  ままなのに実機からの新規出力を一切転送しなくなる(サイレントに詰まる)
  ことがある。実機側は正常に動き続けている(再接続すれば続きのログが
  取れる)。定期的に再起動するか、長時間の連続監視に依存しないテスト設計
  (短い操作単位で確認)にする方が安全。`--no-reset` での再接続はこの環境
  ではむしろ詰まりやすく、通常のリセット付き再接続の方が安定した(8c)。
- ユーザーが実機を触りながら現象を確認したい場合、`esp32-monitor` ペインに
  直接フィルタ済みのライブログを出すと効率的(`idf.py monitor | tee <保存用
  ログ> | grep --line-buffered -E "<pattern>"`)。ホスト側ファイルを都度
  読み上げて報告するより、ユーザー自身がペインを見ながら物理操作できる(8c)。

### Linux ホスト(SDL / GUI 自動化)
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

# 依存の記録

| 依存 | 追加フェーズ | 理由 |
|---|---|---|
| `espressif/wasm-micro-runtime` (registry, 2.4.0 系固定) | Phase 1 | WASM ランタイム本体。registry 経由が既存ビルドフロー(managed_components + Docker + CI)と整合し追加コスト最小 |
| (Linux) WAMR vmlib, SDL2 | Phase 3 | Linux ホスト用。実機と同一ランタイムで API 登録コードを共有するため |
| (Linux) SDL2_ttf(任意) | Phase 5 後 | 実機(LVGL の AA フォント)に見た目を合わせるため。無ければ font8x8 にフォールバックするので必須依存ではない |
| (Linux) SDL2_mixer(任意) | Phase 6B | MP3 再生。対案 mpg123 直叩きはデコード後の PCM 出力経路(デバイス管理・ミキシング)を自作する必要があるのに対し、SDL_mixer は pause/resume/volume/終了フック(Mix_HookMusicFinished)が hostapi_audio_* の状態機械に 1:1 で対応し、既存 SDL2 と同居できる。無ければ audio_play が常に ERROR を返すビルドになる(必須依存ではない) |
| (Linux) ALSA(libasound、任意) | Phase 8b | MIDI OUT(hostapi_midi_send)の実送信。Linux ホストには実 MIDI ポートがないため、ALSA シーケンサ(snd_seq)経由で実機同等の外部 MIDI 機器(UM-ONE 等)へ直接送信できるようにした(ユーザーが Phase 8a で UM-ONE の動作確認環境を Linux ホストに用意していたため採用)。無ければ送信バイト列を stderr にログ出力するだけのフォールバックで動作する(必須依存ではない) |

# ビルドメモ

- Docker: `ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5`(README 参照)。
  `esp32-build` ペインで一発コマンドとして実行。
- `src/` で `idf.py build` / `idf.py flash`。
- CI: `.github/workflows/build.yml` が devcontainer で `idf.py build`。
