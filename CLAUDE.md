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

## 現在地 (2026-07-18 時点)

- Phase 7(7A 予約発音 / 7B メトロノーム / 7B-fix DMA 二重クリック / 7C トーンパレット)完了。
- Zenn 連載: 第 1〜7 回公開済み、第 8〜17 回はスケジュール公開設定済み(〜2026-07-28、詳細は docs/zenn.md)。
- 次の候補: メトロノーム磨き込み(テンポ 1 刻み・ボリューム調整。Host API 変更なしのスコープ)。

## 開発の進め方(このリポジトリでの作業ルール)

- 「小さいターゲットを定めて、テストし、次を計画する」の反復。各フェーズはビルドが通り
  コミット可能な粒度を保つ。
- 開発環境は **herdr**。シェル実行はすべて下記「シェル実行の原則」に従い
  `scripts/hpane.sh` 経由で行う。
- Phase 完了まで、ビルド・フラッシュ・モニタ確認を含めて確認なしで自律的に進めてよい。
  各ステップの結果はログとして **docs/dev-log.md** に残すこと。
  実施記録の冒頭には対応するフェーズ指示書(docs/prompts/phaseXX.md)への参照を書く。
- 実機検証は Web カメラ(/dev/video0)で実機を撮影して行う。録画は `~/ビデオ/rec.sh`
  (露出・フォーカス自動適用、Enter で停止)。`camera` ペインで起動し、空文字送信
  (=Enter)で停止する。動画・静止画は Zenn 記事の素材として `~/ビデオ/zenn-phaseXX/` に残す。
- 依存追加は最小限。追加時は本ファイル末尾「依存の記録」に理由を残す。
- 既存アプリ(demo / bars / mp3player / metronome / clicktest)の回帰を壊さない。
  (旧「MP3 デモモードで分岐」ルールは Phase 6D で解消済み。履歴は docs/dev-log.md 冒頭。)
  
## アーキテクチャ方針(決定済み)

- タイミングクリティカル層(オーディオ出力、描画ドライバ、将来の FM 音源)はネイティブ
  ホスト側。WASM アプリはロジックのみを持ち、ホスト API を叩く。
- WASM アプリは Rust / `wasm32-unknown-unknown`(WASI 不使用)。
- 実機ランタイムは WAMR。まず interpreter で動かし、AOT は後続フェーズ。

## シェル実行の原則

シェルで実行するもの(ビルド、フラッシュ、モニタ、カメラ撮影、動作確認)はすべて、
直接 Bash で実行せず `scripts/hpane.sh` 経由で herdr のペインで実行すること。

- **pane ID を記憶・再利用してはならない。** herdr の pane ID はペインが閉じられると
  詰められるため永続ではない。必ず `hpane.sh` がラベルから毎回解決する。
- **`herdr wait output` をビルドログの文言(例: "Project build complete")に対して
  直接使ってはならない。** 過去のスクロールバックへの誤マッチと文言揺れによる
  タイムアウトの原因になる。完了待ちは必ず `hpane.sh run` の番兵トークン方式で行う。
- `hpane.sh run` の exit code がそのままコマンドの成否である。
  exit 0 以外なら失敗として扱い、`hpane.sh read <name> 100` でログを確認して報告する。
  exit 124 はタイムアウト。

## ペイン一覧(ラベル固定)

| ラベル | 用途 | 実行形式 |
|---|---|---|
| `esp32-build` | ESP32 実機ビルド / フラッシュ | 常に docker を含む一発コマンド(下記) |
| `esp32-monitor` | シリアルモニタ(常駐) | `send` で起動、`waitfor` でログ待ち |
| `unix-build` | Linux ホスト(SDL)ビルド / 実行 | 一発コマンド |
| `camera` | カメラ撮影(ffmpeg / v4l2-ctl) | `send`(常駐)+ `run`(単発) |
| `zenn` | Zenn ドキュメント作成関連 | 一発コマンド |

新しいラベルを増やす場合は事前にユーザーの承認を得ること。

## 実行形式のルール

- **ペインに対話状態を持たせない。** docker コンテナ内での作業も、ペイン内で
  `docker exec` シェルに入ったままにせず、毎回一発コマンドとして実行する。
  これによりペインの状態に依存せず、いつ再開しても同じ手順で再現できる。

```bash
# ESP32 ビルド(例。コンテナ名・パスはプロジェクトの実際の値に合わせる)
./scripts/hpane.sh run esp32-build \
  "docker exec -w /project/src <container> idf.py build" 1800000

# ESP32 フラッシュ
./scripts/hpane.sh run esp32-build \
  "docker exec -w /project/src <container> idf.py -p /dev/ttyACM0 flash" 300000

# Linux ホストビルド
./scripts/hpane.sh run unix-build "cd host_linux && make" 600000

# シリアルモニタ(常駐: run ではなく send + waitfor)
./scripts/hpane.sh send esp32-monitor \
  "docker exec -w /project/src <container> idf.py -p /dev/ttyACM0 monitor"
./scripts/hpane.sh waitfor esp32-monitor "app_main" 30000
./scripts/hpane.sh read esp32-monitor 80
```

## タイムアウト既定値(ms)

| 操作 | timeout |
|---|---|
| ESP32 フルビルド | 1800000 (30分) |
| ESP32 インクリメンタルビルド | 600000 (10分) |
| フラッシュ | 300000 (5分) |
| Linux ホストビルド | 600000 (10分) |
| モニタのログ待ち | 30000〜60000 |

タイムアウトした場合は勝手に次へ進まず、`read` でログを確認して状況を報告し停止すること。

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

### herdr / ビルド
- 完了待ちは hpane.sh の番兵方式のみ。ログ文言への `wait output` 直マッチは
  高速スクロール行を取りこぼす(6C)。
- pane の cwd ドリフトに注意。ビルドは絶対パス+成果物のタイムスタンプ/シンボル確認を
  セットで行う(7C)。

## 初回セットアップ(ユーザーが一度だけ実施)

```bash
# herdr 公式エージェントスキルの導入(Claude Code が herdr 操作を正しく学ぶ)
npx skills add ogulcancelik/herdr --skill herdr -g

# ヘルパー配置
chmod +x scripts/hpane.sh
```

`.claude/settings.local.json` の permissions に以下を追加:

```json
"Bash(herdr:*)",
"Bash(./scripts/hpane.sh:*)"
```

## 初回導入時の確認事項(Claude Code への指示)

`hpane.sh` は `herdr tab list` / `tab get` の JSON 構造をキー名に依存しない形で
走査しているが、初回のみ以下を実行して実際の構造を確認し、
`ensure` が正しい pane ID を返すことを検証してから本作業に入ること:

```bash
herdr tab list --workspace 1
./scripts/hpane.sh ensure unix-build
./scripts/hpane.sh run unix-build "echo hello" 10000
```

問題があれば `hpane.sh` のパーサ部(VERIFY コメント箇所)を実際の JSON に合わせて
修正し、修正内容を報告すること。

# 依存の記録

| 依存 | 追加フェーズ | 理由 |
|---|---|---|
| `espressif/wasm-micro-runtime` (registry, 2.4.0 系固定) | Phase 1 | WASM ランタイム本体。registry 経由が既存ビルドフロー(managed_components + Docker + CI)と整合し追加コスト最小 |
| (Linux) WAMR vmlib, SDL2 | Phase 3 | Linux ホスト用。実機と同一ランタイムで API 登録コードを共有するため |
| (Linux) SDL2_ttf(任意) | Phase 5 後 | 実機(LVGL の AA フォント)に見た目を合わせるため。無ければ font8x8 にフォールバックするので必須依存ではない |
| (Linux) SDL2_mixer(任意) | Phase 6B | MP3 再生。対案 mpg123 直叩きはデコード後の PCM 出力経路(デバイス管理・ミキシング)を自作する必要があるのに対し、SDL_mixer は pause/resume/volume/終了フック(Mix_HookMusicFinished)が hostapi_audio_* の状態機械に 1:1 で対応し、既存 SDL2 と同居できる。無ければ audio_play が常に ERROR を返すビルドになる(必須依存ではない) |

# ビルドメモ

- Docker: `ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5`(README 参照)。
  `esp32-build` ペインで一発コマンドとして実行。
- `src/` で `idf.py build` / `idf.py flash`。
- CI: `.github/workflows/build.yml` が devcontainer で `idf.py build`。
