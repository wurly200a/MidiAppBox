# wasm-apps

MidiAppBox 上で動かす WASM アプリ(Rust, `wasm32-unknown-unknown`, no_std)。

ビルド済み `.wasm` はファームウェアに埋め込むためリポジトリにコミットする
(ESP-IDF ビルダーコンテナに Rust が無いため、ビルドはホスト側で行う)。

## 必要なもの

```
rustup target add wasm32-unknown-unknown
```

## ビルド手順(hello の例)

```
cd wasm-apps/hello
cargo build --release
cp target/wasm32-unknown-unknown/release/hello.wasm ./hello.wasm
```

`hello.wasm` が `src/components/wasm_runtime` の CMake から EMBED_FILES で参照される。
