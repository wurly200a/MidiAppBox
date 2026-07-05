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
