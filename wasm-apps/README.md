# wasm-apps

MidiAppBox 上で動かす WASM アプリ(Rust, `wasm32-unknown-unknown`, no_std)。

ビルド済み `.wasm` はファームウェアに埋め込むためリポジトリにコミットする
(ESP-IDF ビルダーコンテナに Rust が無いため、ビルドはホスト側で行う)。

## 必要なもの

```
rustup target add wasm32-unknown-unknown
```

## ビルド手順(hello / demo 共通)

```
cd wasm-apps/<app>
cargo build --release
cp target/wasm32-unknown-unknown/release/<app>.wasm ./<app>.wasm
```

各 `<app>.wasm` が `src/components/wasm_runtime` の CMake から EMBED_FILES で参照される。

## アプリ一覧

| アプリ | 内容 |
|---|---|
| `hello/` | Phase 1 の最小テスト。`app_init()` が 42 を返すだけ |
| `demo/` | Phase 2 デモ。ホスト API で 1 秒ごとにカウンタ描画+クリック音 |
| `bars/` | Phase 5B デモ。イコライザ風 8 本バー(座標固定・サイズ/色可変) |
| `bench/` | Phase 4 計測用。`bench_empty`/`bench_hostcall`(ランチャーからは起動不可) |
| `touch_demo/` | Phase 6A 検証。`hostapi_poll_event` のタッチイベントを座標・DOWN/UP カウントで可視化、ボタンタップでクリック音 |
| `mp3player/` | Phase 6B〜。`hostapi_audio_*` で MP3 を制御(PLAY/PAUSE/STOP/VOL±、FINISHED 検知)。6C でファイル列挙+プレイリスト対応 |
| `clicktest/` | Phase 7A 検証。`hostapi_click_schedule` で BPM120 を予約発音。タップで SCHED⇔LEGACY(tick 内直呼び)を切替してジッタ比較 |
