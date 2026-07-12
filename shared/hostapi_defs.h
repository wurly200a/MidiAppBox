/*
 * ホスト API の定義(実機 ESP32 ホストと Linux ホストで共有)。
 *
 * X(name, signature):
 *   name      = wasm import 名(module "env")。ネイティブ実装は native_<name>。
 *   signature = WAMR native symbol シグネチャ。
 *               "*~" は (ptr, len) ペアで、WAMR が境界検証済みポインタに変換する。
 *
 * API 契約:
 *   画面座標系はランドスケープ 320x240(左上原点)。
 *   hostapi_draw_text(x, y, str_ptr, str_len)  UTF-8 文字列を (x,y) に描画。
 *                                              同一座標への再描画は置き換え。
 *   hostapi_fill_rect(x, y, w, h, rgb888)      矩形塗り。色は 0xRRGGBB。
 *                                              同一 (x,y) への再描画は置き換え。
 *   hostapi_play_click()                       クリック音(短い減衰サイン)を再生。
 *   hostapi_now_ms() -> u32                    起動からの経過ミリ秒。
 *
 *   hostapi_poll_event(buf_ptr, buf_len) -> n  (Phase 6A)
 *     前回呼び出し以降の入力イベントを buf に書き、書いた件数を返す(0=なし)。
 *     buf は hostapi_event_t の配列(buf_len はバイト数)。ホストは
 *     buf_len / sizeof(hostapi_event_t) 件を上限に書き、入り切らない分は
 *     キューに残して次回返す。戻り値の負数はエラー用に予約(当面未使用)。
 *     アプリは tick 先頭で drain する想定。推奨バッファは 16 件分。
 *
 *   hostapi_audio_play(path_ptr, path_len) -> 0/-1  (Phase 6B)
 *     MP3 の再生を開始する。path は「ミュージックルート相対」
 *     (実機 /sdcard/music/、Linux ./sdcard/music/)。".." を含む・"/" で
 *     始まるパスは拒否する(サンドボックス境界)。再生中に呼ぶと現在の曲を
 *     止めて差し替える。成功 0(state=PLAYING)、失敗 -1(state=ERROR)。
 *   hostapi_audio_ctrl(cmd) -> 0/-1
 *     HOSTAPI_AUDIO_CMD_*。現在の状態で無効なコマンド(停止中の PAUSE 等)は
 *     何もせず -1(トラップしない)。STOP は任意の状態から STOPPED へ。
 *   hostapi_audio_set_volume(v)
 *     音量 0..100(範囲外はクランプ)。曲をまたいで持続する。
 *   hostapi_audio_get_state() -> HOSTAPI_AUDIO_*
 *     FINISHED(自然終了)は読み取りでは消えず、次の play か STOP まで保持。
 *     ERROR も同様に次の play まで保持。
 *
 *   オーディオのライフサイクル契約: アプリ起動時は STOPPED。アプリ破棄時、
 *   ホストは再生中のオーディオを必ず停止する。
 *
 * 入力イベント規約(v1 で凍結する ABI):
 *   - レコードは 12 バイト固定・リトルエンディアン(WASM 仕様と一致)。
 *     サイズ変更は破壊的変更なので行わない。拡張は type の追加
 *     (アプリは未知の type を無視する契約)と param への型依存値で行う。
 *   - ホスト側キューは深さ 16。溢れたら最古から捨てる(警告ログ)。
 *     キューはアプリ起動時に空で始まり、アプリ破棄で消える。
 *   - 対応する DOWN を配送していない UP はホストが捨てる(アプリを起動した
 *     タップの UP がアプリに漏れないように)。アプリ側も DOWN なしの UP は
 *     無視してよい。
 *   - v1 はシングルタッチ(マルチタッチは将来 param=finger id で拡張)。
 */
#pragma once

#include <stdint.h>

/* 12 bytes, align 4。フィールドはリトルエンディアン */
typedef struct {
    uint16_t type;    /* HOSTAPI_EV_* */
    uint16_t param;   /* type 依存の追加値。TOUCH_* では 0 */
    int16_t  x;       /* 論理画面座標(320x240 左上原点)。非タッチ系では 0 */
    int16_t  y;
    uint32_t time_ms; /* イベント発生時刻。hostapi_now_ms() と同一時基 */
} hostapi_event_t;

enum {
    HOSTAPI_EV_NONE       = 0, /* 予約(無効値) */
    HOSTAPI_EV_TOUCH_DOWN = 1,
    HOSTAPI_EV_TOUCH_UP   = 2,
    /* 将来: TOUCH_MOVE, KEY, ... 追加は非破壊 */
};

/* hostapi_audio_ctrl のコマンド (Phase 6B) */
enum {
    HOSTAPI_AUDIO_CMD_PAUSE  = 1,
    HOSTAPI_AUDIO_CMD_RESUME = 2,
    HOSTAPI_AUDIO_CMD_STOP   = 3,
};

/* hostapi_audio_get_state の状態 (Phase 6B) */
enum {
    HOSTAPI_AUDIO_STOPPED  = 0,
    HOSTAPI_AUDIO_PLAYING  = 1,
    HOSTAPI_AUDIO_PAUSED   = 2,
    HOSTAPI_AUDIO_FINISHED = 3, /* 自然終了。次の play か STOP まで保持 */
    HOSTAPI_AUDIO_ERROR    = 4, /* play 失敗。次の play まで保持 */
};

#define HOSTAPI_NATIVE_SYMBOLS(X)         \
    X(hostapi_draw_text, "(ii*~)")        \
    X(hostapi_fill_rect, "(iiiii)")       \
    X(hostapi_play_click, "()")           \
    X(hostapi_now_ms, "()i")              \
    X(hostapi_poll_event, "(*~)i")        \
    X(hostapi_audio_play, "(*~)i")        \
    X(hostapi_audio_ctrl, "(i)i")         \
    X(hostapi_audio_set_volume, "(i)")    \
    X(hostapi_audio_get_state, "()i")

/* NativeSymbol 配列の初期化子を生成するヘルパ */
#define HOSTAPI_SYMBOL_ENTRY(name, sig) { #name, (void*)native_##name, sig, NULL },
