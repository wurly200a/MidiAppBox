# wasm-apps

MidiAppBox 上で動かす WASM アプリ(Rust, `wasm32-unknown-unknown`, no_std)。

ビルド済み `.wasm` はファームウェアに埋め込むためリポジトリにコミットする
(ESP-IDF ビルダーコンテナに Rust が無いため、ビルドはホスト側で行う)。

## 必要なもの

```
rustup target add wasm32-unknown-unknown
```

## ビルド手順(全アプリ共通)

```
cd wasm-apps/<app>
cargo build --release
cp target/wasm32-unknown-unknown/release/<app>.wasm ./<app>.wasm
```

各 `<app>.wasm` が `src/components/wasm_runtime` の CMake から EMBED_FILES で参照される。

## アプリ一覧

| アプリ | 内容 |
|---|---|
| `touch_demo/` | Phase 6A 検証。`hostapi_poll_event` のタッチイベントを座標・DOWN/UP カウントで可視化、ボタンタップでクリック音 |
| `mp3player/` | Phase 6B〜。`hostapi_audio_*` で MP3 を制御(PLAY/PAUSE/STOP/VOL±、FINISHED 検知)。6C でファイル列挙+プレイリスト対応 |
| `metronome/` | Phase 7B/7C/7D → **Phase 13 で音楽時間軸 API に全面書き直し**。メトロノーム本体。可変 BPM(40-240、±5/±1・長押し連打加速)・拍子(2/3/4/6)・START/STOP・拍ランプ・音量調整(V-/V+)。クリックは `seq_write`(port=CLICK / OP_TONE)で playback tick に予約し、MIDI Clock は L1 がグリッドから生成する(アプリは `hostapi_midi_send` を呼ばない)。小節頭は 1568Hz のアクセント音 |
| `midi_loopback/` | Phase 9b → **Phase 14 で音楽時間軸 API に移行**。MIDI ループバック診断アプリ。`hostapi_transport_start/stop` + `tempomap_set_tempo/meter`(BPM120固定)で自機 MIDI OUT の 24ppqn クロックを駆動し、`hostapi_midi_recv` で自機 MIDI IN の受信を診断表示(Stage1: 受信生バイトの16進表示・累積バイト数、Stage2: 実測 BPM、Stage3: クロック間隔の min/max/σ・公称値との偏差・受信数 vs 期待数、E1: ヒストグラム・ロバスト統計・外れ値・見かけBPM分布)。可聴クリックは供給しない(受信統計に条件を絞る判断) |
| `seq_smoke/` | Phase 11。新 Host API 12 関数(transport / tempomap / seq / time_us_to_tick)の恒久スモークテスト。タップ不要で一巡し、8 項目の合否をビットで画面表示 + CC#119/#120 で外部出力する。実機と Linux ホストで同一 `.wasm` を走らせて比較できる |
