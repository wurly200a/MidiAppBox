#pragma once
// L0(tick 順イベントキュー + ポート抽象)/ L1(音楽時間軸)。Phase 11 ステップ 1。
// 設計は docs/architecture.md §4〜§7 / §9、API 仕様は docs/hostapi-next.md。
//
// 本ヘッダは native 内部インタフェースであり、WASM から見える Host API
//(hostapi_transport_* 等)はステップ 2 で hostapi.cpp が薄く委譲する。
//
// 時刻源は clockauth::NowUs() ただ 1 つ。ディスパッチは esp_timer ワンショット
// 1 本で、再アームは常に**絶対時刻グリッド基準**(予定 tick → host µs)である。
// 発火時刻からの相対加算は行わない(累積ドリフト防止、P10-3 で実証済みの方式)。
//
// ロック規律(§6、必須): 本モジュールは専用の短い spinlock だけを使い、
// LVGL / ファイルシステム / オーディオ書き込みのロックとは一切共有しない。
// 臨界区間はキューとタイムラインの更新に限定し、その中で描画・I/O・ログ出力・
// esp_timer 操作を行わない(ポートへの送出は必ずロックの外)。
#include <cstddef>
#include <cstdint>

namespace seq {

// 内部 PPQN(§4)。MIDI Clock は 40 tick グリッド(960/24)。
constexpr uint32_t kPpqn = 960;
constexpr uint32_t kClockGridTicks = kPpqn / 24;

// transport 状態
enum {
    kStopped = 0,
    kPlaying = 1,
};

// イベントの出力先ポート(§7)。USB_MIDI / SYNTH は予約のみ。
enum {
    kPortDinOut  = 0,
    kPortUsbMidi = 1,
    kPortSynth   = 2,
    kPortClick   = 3,
};

// status が MIDI ステータスバイト(0x80 以上)でない場合の内部オペコード
enum {
    kOpNone = 0,
    kOpTone = 1, // port=CLICK。param = トーンスロット (0..7)
};

// L0 のキュー要素。16 バイト。ステップ 2 で shared/hostapi_defs.h の
// hostapi_seq_event_t と同一レイアウトであることを static_assert で固定する
//(境界での変換コストをゼロにするため。§6)。
typedef struct {
    uint32_t tick;      // 発火する playback tick(絶対)
    uint8_t  port;      // kPort*
    uint8_t  status;    // MIDI ステータスバイト、または kOp*
    uint8_t  data1;
    uint8_t  data2;
    uint32_t param;     // op 依存(tone slot 等)。MIDI イベントでは 0
    uint32_t _reserved; // 常に 0
} l0_event_t;

static_assert(sizeof(l0_event_t) == 16, "l0_event_t must be 16 bytes");

// transport 位置。32 バイト。hostapi_position_t と同一レイアウト。
typedef struct {
    uint64_t host_us;
    uint32_t tick;         // playback tick
    uint32_t song_tick;
    uint32_t bar;
    uint32_t tempo_upq;
    uint16_t beat;
    uint16_t tick_in_beat;
    uint32_t state;
} l1_position_t;

static_assert(sizeof(l1_position_t) == 32, "l1_position_t must be 32 bytes");

// CLICK ポートの発音ハンドラ。トーンパレット(hostapi.cpp のアプリセッション
// 状態)はホスト API 側にあるので、コンポーネント間の循環依存を避けるために
// コールバックで受け取る。呼び出しは esp_timer タスク上・ロック外。
using ClickHandler = void (*)(uint32_t slot);
void SetClickHandler(ClickHandler fn);

// 起動時に 1 回(app_main から。midi::Midi_Init の後)。
void Init();

// アプリのライフサイクルに合わせて初期状態へ戻す(hostapi_audio_reset から)。
// transport を停止し、キュー・テンポマップ・ループ設定を破棄する。
void Reset();

// ---- L1: transport(docs/hostapi-next.md §3)----
int32_t TransportStart();
int32_t TransportStop();
int32_t TransportContinue();
int32_t TransportLocate(uint32_t song_tick);
int32_t TransportGetPosition(void* buf, size_t buf_len);

// ---- L1: tempomap(同 §4)----
int32_t TempoMapSetTempo(uint32_t at_song_tick, uint32_t us_per_quarter);
int32_t TempoMapSetMeter(uint32_t at_song_tick, uint32_t numer, uint32_t denom);
int32_t TempoMapSetLoop(uint32_t start_song_tick, uint32_t end_song_tick);

// ---- L0: seq(同 §5)----
// SeqWrite は先頭から連続した n 件(プレフィックス)のみを受理する。
// 残りはアプリが保持して再送する契約(architecture.md §11-9)。
int32_t SeqWrite(const void* buf, size_t buf_len);
int32_t SeqFlushAfter(uint32_t tick);
int32_t SeqFilledUntil();

// ---- time(同 §9)----
int32_t TimeUsToTick(int64_t us);

#ifdef PHASE11_L0_SELFTEST
// 起動時の自己検査(tick 順・安定順序・満杯時の受理数・flush_after の件数)。
void SelfTest();
#endif

} // namespace seq
