# アーキテクチャ方針(決定済み)

- タイミングクリティカル層(オーディオ出力、描画ドライバ、将来の FM 音源)はネイティブ
  ホスト側。WASM アプリはロジックのみを持ち、ホスト API を叩く。
- WASM アプリは Rust / `wasm32-unknown-unknown`(WASI 不使用)。
- 実機ランタイムは WAMR。まず interpreter で動かし、AOT は後続フェーズ。
