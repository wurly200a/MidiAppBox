/*
 * ホスト API v0 の定義(実機 ESP32 ホストと Linux ホストで共有)。
 *
 * X(name, signature):
 *   name      = wasm import 名(module "env")。ネイティブ実装は native_<name>。
 *   signature = WAMR native symbol シグネチャ。
 *               "*~" は (ptr, len) ペアで、WAMR が境界検証済みポインタに変換する。
 *
 * API 契約:
 *   hostapi_draw_text(x, y, str_ptr, str_len)  UTF-8 文字列を (x,y) に描画。
 *                                              同一座標への再描画は置き換え。
 *   hostapi_fill_rect(x, y, w, h, rgb888)      矩形塗り。色は 0xRRGGBB。
 *                                              同一 (x,y) への再描画は置き換え。
 *   hostapi_play_click()                       クリック音(短い減衰サイン)を再生。
 *   hostapi_now_ms() -> u32                    起動からの経過ミリ秒。
 */
#pragma once

#define HOSTAPI_NATIVE_SYMBOLS(X)      \
    X(hostapi_draw_text, "(ii*~)")     \
    X(hostapi_fill_rect, "(iiiii)")    \
    X(hostapi_play_click, "()")        \
    X(hostapi_now_ms, "()i")

/* NativeSymbol 配列の初期化子を生成するヘルパ */
#define HOSTAPI_SYMBOL_ENTRY(name, sig) { #name, (void*)native_##name, sig, NULL },
