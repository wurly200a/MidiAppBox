/*
 * ホスト API v1 の定義(実機 ESP32 ホストと Linux ホストで共有)。
 *
 * X(name, signature):
 *   name      = wasm import 名(module "env")。ネイティブ実装は native_<name>。
 *   signature = WAMR native symbol シグネチャ。
 *               "*~" は (ptr, len) ペアで、WAMR が境界検証と app→native の
 *               アドレス変換を行う(in/out どちらのバッファにも使う)。
 *
 * ============================== 共通契約 ==============================
 *
 * - 画面座標系: ランドスケープ 320x240、左上原点、単位ピクセル。
 * - 文字列 in 引数: (ptr, len) の UTF-8。NUL 終端不要。
 * - out-buffer: アプリが (buf_ptr, buf_len) を渡し、ホストが書いた量
 *   (件数または長さ)を戻り値で返す。ホストは buf_len を超えて書かない。
 * - エラーを返す関数は負数(通常 -1)。アプリの不正入力でトラップさせない。
 * - アプリのライフサイクル: app_init() → 100ms 周期の app_tick() 反復 →
 *   (任意 export の app_exit())→ ホストが破棄。すべて同一スレッド。
 *   破棄時、ホストは再生中のオーディオを必ず停止する。
 *   アプリ起動時: 描画スロットは空、イベントキューは空、audio は STOPPED。
 *
 * ============================== gfx ==============================
 *
 * 描画は (x,y) をキーにした retained モデル。同一座標への再描画は
 * 既存オブジェクトの置き換え(移動・部分消去の API はない)。
 * スロットは text / rect 各 16。あふれは警告ログの上で無視される。
 *
 *   hostapi_draw_text(x, y, str_ptr, str_len)
 *     UTF-8 文字列を (x,y) に描画。色は白固定(v1)。
 *   hostapi_fill_rect(x, y, w, h, rgb888)
 *     矩形塗り。色は 0xRRGGBB。
 *
 * ============================== input ==============================
 *
 *   hostapi_poll_event(buf_ptr, buf_len) -> n
 *     前回呼び出し以降の入力イベントを buf に書き、書いた件数を返す(0=なし)。
 *     buf は hostapi_event_t の配列(buf_len はバイト数)。ホストは
 *     buf_len / 12 件を上限に書き、入り切らない分はキューに残して次回返す。
 *     アプリは tick 先頭で drain する想定。推奨バッファは 16 件分。
 *
 *   イベント規約(ABI 凍結):
 *     - hostapi_event_t は 12 バイト固定・リトルエンディアン。サイズ変更は
 *       しない。拡張は type の追加(アプリは未知 type を無視する契約)と
 *       param への型依存値で行う。
 *     - ホスト側キューは深さ 16。溢れたら最古から捨てる。
 *     - 対応する DOWN を配送していない UP はホストが捨てる(アプリを起動
 *       したタップの UP が漏れないように)。アプリ側も DOWN なしの UP は
 *       無視してよい。
 *     - v1 はシングルタッチ(マルチタッチは将来 param=finger id で拡張)。
 *
 * ============================== audio ==============================
 *
 * MP3 のデコード・出力はネイティブ側。アプリは制御のみを持つ。
 * path は「ミュージックルート相対」(実機 /sdcard/music/、Linux
 * ./sdcard/music/)。".." を含む・"/" で始まるパスは拒否(サンドボックス境界)。
 *
 *   hostapi_audio_play(path_ptr, path_len) -> 0/-1
 *     再生開始。再生中に呼ぶと現在の曲を止めて差し替える。
 *     成功 0(state=PLAYING)、失敗 -1(state=ERROR)。
 *   hostapi_audio_ctrl(cmd) -> 0/-1
 *     HOSTAPI_AUDIO_CMD_*。現在の状態で無効なコマンド(停止中の PAUSE 等)
 *     は何もせず -1。STOP は任意の状態から STOPPED へ。
 *   hostapi_audio_set_volume(v)
 *     マスター音量 0..100(範囲外はクランプ)。MP3 とクリックの両方に適用
 *     (v2 で「MP3 の音量」から再定義)。曲をまたいで持続する。
 *     発音中のクリックには効かず、次の発音から有効。
 *   hostapi_audio_get_state() -> HOSTAPI_AUDIO_*
 *     FINISHED(自然終了)は読み取りでは消えず、次の play か STOP まで保持
 *     (100ms tick のポーリングで取りこぼさないため)。ERROR も同様。
 *
 * ============================== fs ==============================
 *
 *   hostapi_fs_list(idx, buf_ptr, buf_len) -> n
 *     ミュージックルート直下の .mp3 ファイル名を idx(0 始まり)で列挙する。
 *     名前(ルート相対、NUL 終端なし)を buf に書き、その長さを返す。
 *     idx が範囲外なら -1(終端)。63 バイトを超える名前とサブディレクトリは
 *     列挙から除外。列挙順はホスト依存だが同一セッション中は安定。
 *     返る名前はそのまま hostapi_audio_play に渡せる。
 *
 * ============================== misc ==============================
 *
 *   hostapi_play_click()   クリック音(短い減衰サイン)を即時再生。
 *                          MP3 再生との同時使用は将来の音源 API で整理予定。
 *   hostapi_now_ms() -> u32  起動からの経過ミリ秒(イベントの time_ms と同一時基)。
 *
 *   hostapi_tone_define(slot, wave, freq_hz, dur_ms, level) -> 0/-1  (Phase 7C, v2)
 *     slot(0..7)に短い減衰音をパラメトリックに定義する(再定義可)。
 *     wave: HOSTAPI_WAVE_*(v2 は SINE のみ。未知の値は -1、トラップしない)
 *     freq_hz 100..8000 / dur_ms 5..100 / level 0..100(範囲外はクランプ)。
 *     level はトーン固有ゲインで、マスター音量と乗算される。
 *     エンベロープは指数減衰(dur_ms 終端で約 -30dB)。
 *     定義はアプリセッション状態: 起動時 slot 0 = 既定クリック
 *     (1000Hz/30ms/100)、slot 1..7 = 未定義。破棄で消滅。
 *   hostapi_tone_play(slot) -> 0/-1
 *     即時発音。未定義スロットは -1。
 *   hostapi_tone_schedule(slot, time_ms) -> 0/-1
 *     予約発音。予約の契約は hostapi_click_schedule と共通(下記)で、
 *     予約はスロットによらず全体で 1 件。パラメータは予約時にスナップショット
 *     される(発音前に tone_define し直しても発音済み予約には影響しない)。
 *   発音の重なり(前の音が鳴り終わる前の発音)は v2 ではベストエフォート
 *   (単声。実機は直列再生)。ポリフォニーとサンプル再生は将来の音源 API で扱う。
 *
 *   hostapi_play_click()          ≡ hostapi_tone_play(0)
 *   hostapi_click_schedule(t)     ≡ hostapi_tone_schedule(0, t)
 *     (v0/7A 互換。slot 0 を再定義すればこれらの音も変わる)
 *
 * ============================== misc ==============================
 *
 *   hostapi_click_schedule(time_ms) -> 0/-1  (Phase 7A, v2)
 *     time_ms(hostapi_now_ms() と同一時基)にクリック音を発音するよう予約する。
 *     tick 格子(100ms)より細かいタイミング精度が要る発音のための API
 *     (タイミングクリティカルはネイティブ側、の原則によりスケジューリングを
 *     ホストに移す)。
 *     - 予約はホスト側に常に 1 件のみ。呼ぶたびに置き換える。
 *     - time_ms == 0 は予約キャンセル。
 *     - ホストは最後に発音した予約時刻 last_fired を保持し、
 *       time_ms <= last_fired の予約は無視して 0 を返す(冪等な再予約を許す)。
 *       アプリは毎 tick「次の拍」を再予約するだけでよく、二重発音しない。
 *     - now を過ぎた時刻(ただし last_fired より後)の予約は可及的速やかに発音。
 *     - アプリ破棄時、ホストは予約と last_fired をリセットする。
 *     - v2 では MP3 再生中の予約発音の精度は保証しない(クリックと MP3 は排他前提)。
 *
 * ============================== midi ==============================
 *
 *   hostapi_midi_send(bytes_ptr, bytes_len) -> 0/-1  (Phase 8b)
 *     MIDI バイト列をそのまま MIDI OUT へ送信する(App drives API: App は
 *     素の MIDI バイト列を渡すだけで、ホストはその意味を強制しない)。
 *     bytes_len は 1..8(範囲外は -1)。MIDI OUT 未初期化でも -1。
 *
 *     ただし System Realtime の Start(0xFA)/Continue(0xFB)/Stop(0xFC) を
 *     **単独の1バイトメッセージ**として送ると、ホストはそれをトリガに
 *     内部で 24ppqn の MIDI Clock(0xF8)の生成を開始/停止する。
 *     - クロック生成は app_tick に一切依存しない、host 内部のタイマ駆動。
 *     - テンポは新規に App から伝達しない。既存のクリック/トーン予約
 *       (hostapi_tone_schedule 系, 上記)が毎拍再予約される際の「直前に
 *       発音した時刻」と「次に予約された時刻」の差分から host が導出する。
 *       そのため MIDI Clock は「クリック音を発音している拍の間隔」に
 *       自動的に追従する(二重にテンポを持たない)。
 *     - アプリ破棄時、ホストは MIDI Clock 生成を必ず停止する
 *       (クリック予約のリセットと同じタイミング)。
 *     - Song Position Pointer 等、Continue を位置復帰として使う高度な
 *       同期はスコープ外(v1 では Continue は Start と同じ扱い)。
 *
 *   hostapi_midi_recv(buf_ptr, buf_len) -> n  (Phase 9a)
 *     前回呼び出し以降に MIDI IN で受信した生バイトを、ホストが受信直後に
 *     打ったタイムスタンプ付きで返す。buf は hostapi_midi_recv_t の配列
 *     (buf_len はバイト数)。ホストは buf_len / 16 件を上限に書き、
 *     入り切らない分はリングバッファに残して次回返す(0 = なし)。
 *     hostapi_poll_event と同じ「out-buffer + 件数を返す」系のシグネチャで、
 *     不正なポインタは WAMR の境界検証でトラップされるため native 実装は
 *     実質的に常に 0 以上を返す(バッファ不足は 0 件)。
 *
 *     パース(ランニングステータス解釈、SysEx 組み立て、Clock 検出など)は
 *     一切行わない。生バイト + タイムスタンプのみを渡す(パースは
 *     Phase 9b、アプリ側の責務)。
 *
 *     ホスト内部のリングバッファは 256 件。溢れたら最古を捨てる
 *     (hostapi_poll_event の入力イベントキューと同じ割り切り。
 *     アプリ側へのオーバーフロー通知 API はない)。
 *
 *     タイムスタンプの時間軸: 実機は esp_timer_get_time()(µs、MIDI Clock
 *     送信側と同一時間軸)、Linux は起動基準の単調増加 µs クロック
 *     (SDL_GetPerformanceCounter 由来。実機の esp_timer とは epoch が異なる
 *     が、レコード間の差分計算にのみ使う前提なので単調増加であれば十分)。
 *     受信 UART/ALSA イベント直後に打刻するため、app_tick の ~5ms ジッタの
 *     影響を受けない。
 *
 *     タイムスタンプ分解能の注意: 1 回の受信イベントに複数バイトが
 *     まとまった場合(ランニングステータスなしで連続送信される
 *     マルチバイトメッセージ等、バイト間に idle gap がない場合)、それらは
 *     同一の代表時刻(≈最後のバイトの到達時刻)を持つ。バイト間に十分な
 *     idle gap がある単発メッセージ(MIDI Clock 0xF8 等の System Realtime
 *     単独送信)はバイト単位に近い精度でタイムスタンプが付く。実機の UART
 *     RX FIFO 閾値/timeout は Phase 8c の検証済み設定から変更していない
 *     (詳細は docs/dev-log.md Phase 9a 節)。
 *
 *     実機: UART1 RX(GPIO15, TLP2361 受信回路, Phase 8c で検証済み)の
 *     UART イベントタスクで受信直後に打刻し、専用リングバッファへ積む。
 *     Linux: ALSA シーケンサ(snd_seq)経由で実受信する(hostapi_midi_send
 *     と同じ、名前に "UM-ONE" を含むポートへの自動接続。見つからない/
 *     ALSA が使えない環境では常に 0 件を返す)。
 *
 *     アプリ破棄時、ホストは受信リングバッファを破棄する(クリック予約・
 *     MIDI Clock 生成のリセットと同じタイミング)。
 */
#pragma once

#include <stdint.h>

/* 入力イベント。12 bytes, align 4。フィールドはリトルエンディアン(ABI 凍結) */
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

/* MIDI IN 受信レコード。16 bytes, align 8。フィールドはリトルエンディアン
 * (ABI 凍結、Phase 9a)。_reserved は常に 0、将来拡張用でサイズ変更はしない。 */
typedef struct {
    uint64_t timestamp_us; /* ホストが受信直後に打った時刻(µs、単調増加) */
    uint8_t  byte;          /* 受信バイト1個 */
    uint8_t  _reserved[7];
} hostapi_midi_recv_t;

/* hostapi_audio_ctrl のコマンド */
enum {
    HOSTAPI_AUDIO_CMD_PAUSE  = 1,
    HOSTAPI_AUDIO_CMD_RESUME = 2,
    HOSTAPI_AUDIO_CMD_STOP   = 3,
};

/* hostapi_audio_get_state の状態 */
enum {
    HOSTAPI_AUDIO_STOPPED  = 0,
    HOSTAPI_AUDIO_PLAYING  = 1,
    HOSTAPI_AUDIO_PAUSED   = 2,
    HOSTAPI_AUDIO_FINISHED = 3, /* 自然終了。次の play か STOP まで保持 */
    HOSTAPI_AUDIO_ERROR    = 4, /* play 失敗。次の play まで保持 */
};

/* hostapi_tone_define の波形 (Phase 7C) */
enum {
    HOSTAPI_WAVE_SINE = 0, /* 減衰サイン(v2 で唯一) */
    /* 将来: NOISE, SQUARE, ... 追加は非破壊 */
};
#define HOSTAPI_TONE_SLOTS 8

/* v1 シンボル一覧(グループ: gfx / input / audio / fs / misc)。
 * v0 の 4 関数(draw_text, fill_rect, play_click, now_ms)はシグネチャ・
 * 挙動とも v0 から不変。 */
#define HOSTAPI_NATIVE_SYMBOLS(X)         \
    /* gfx */                             \
    X(hostapi_draw_text, "(ii*~)")        \
    X(hostapi_fill_rect, "(iiiii)")       \
    /* input */                           \
    X(hostapi_poll_event, "(*~)i")        \
    /* audio */                           \
    X(hostapi_audio_play, "(*~)i")        \
    X(hostapi_audio_ctrl, "(i)i")         \
    X(hostapi_audio_set_volume, "(i)")    \
    X(hostapi_audio_get_state, "()i")     \
    /* fs */                              \
    X(hostapi_fs_list, "(i*~)i")          \
    /* misc / tone */                     \
    X(hostapi_play_click, "()")           \
    X(hostapi_now_ms, "()i")              \
    X(hostapi_click_schedule, "(i)i")     \
    X(hostapi_tone_define, "(iiiii)i")    \
    X(hostapi_tone_play, "(i)i")          \
    X(hostapi_tone_schedule, "(ii)i")     \
    /* midi (Phase 8b / 9a) */             \
    X(hostapi_midi_send, "(*~)i")         \
    X(hostapi_midi_recv, "(*~)i")

/* NativeSymbol 配列の初期化子を生成するヘルパ */
#define HOSTAPI_SYMBOL_ENTRY(name, sig) { #name, (void*)native_##name, sig, NULL },
