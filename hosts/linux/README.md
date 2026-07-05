# Linux 最小ホスト

実機と同一の `.wasm` を同一のホスト API(`shared/hostapi_defs.h`)で動かす
検証用ホスト。ランタイムは実機と同じ WAMR 2.4.0(fast interpreter、
libc builtin のみ)を FetchContent で取得してビルドする。

依存: cmake (>=3.16), gcc, libsdl2-dev

```
cd hosts/linux
cmake -B build
cmake --build build -j
./build/midibox_host ../../wasm-apps/demo/demo.wasm
```

- 描画: SDL2 ウィンドウ(240x320 の 2 倍拡大)+ font8x8(public domain)
- 音: SDL audio に実機と同じ生成 PCM(1kHz 減衰サイン 30ms)
- 終了: ウィンドウを閉じる
