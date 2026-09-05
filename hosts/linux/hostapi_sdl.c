/*
 * ホスト API v0 実装(Linux / SDL2 バックエンド)。
 *
 * 実機側 (src/components/wasm_runtime/hostapi.cpp) と同じ retained モデル:
 * (x,y) をキーに text / rect のスロットを保持し、同一座標への再描画は置き換え。
 * 毎 tick、host_sdl_render() が全スロットを描き直す(rect 群→text 群の順)。
 *
 * テキストは font8x8 (public domain) の 8x8 ビットマップで描画。
 * クリック音は実機と同じ 1kHz 減衰サイン 30ms を SDL のキューへ書く。
 */
#include "hostapi_sdl.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef HAVE_SDL_TTF
#include <SDL_ttf.h>
#endif

#ifdef HAVE_SDL_MIXER
#include <SDL_mixer.h>
#endif

#include "font8x8_basic.h"
#include "hostapi_defs.h"
#include "hostapi_midi.h"
#include "hostapi_seq.h"

/* 実機と同じランドスケープ 320x240 */
#define SCREEN_W 320
#define SCREEN_H 240
#define WINDOW_SCALE 2

#define MAX_TEXT_SLOTS 16
#define MAX_RECT_SLOTS 16
#define MAX_TEXT_LEN 63
#define EVENT_QUEUE_DEPTH 16

typedef struct {
    bool used;
    int32_t x, y;
    char text[MAX_TEXT_LEN + 1];
} TextSlot;

typedef struct {
    bool used;
    int32_t x, y, w, h;
    uint32_t rgb888;
} RectSlot;

static SDL_Window* s_window;
static SDL_Renderer* s_renderer;
static SDL_AudioDeviceID s_audio;
static uint32_t s_start_ms;

/* ---- クリック音のコールバックミキサ (Phase 7A) ----
 * v0 の SDL_QueueAudio(push 型)では発音タイミングがポーリング周期に縛られる
 * ため、クリック用デバイスをコールバック(pull)型に変更。再生済みフレーム数を
 * 音声クロックとして扱い、予約時刻(now_ms 時基)を目標サンプル位置に換算して
 * バッファ内オフセットでサンプル精度の発音を行う。
 * 共有状態は SDL_LockAudioDevice(コールバックは暗黙にロック保持)で保護。 */
#define CLICK_RATE 44100

/* トーンパレット (Phase 7C)。アプリセッション状態(audio_reset で初期化) */
typedef struct {
    bool defined;
    uint16_t freq_hz;
    uint16_t dur_ms;
    uint8_t level;
} ToneDef;
static ToneDef s_tones[HOSTAPI_TONE_SLOTS];
static const ToneDef kDefaultClick = {true, 1000, 30, 100};

/* 発音中のボイス(キャッシュレス合成: 再帰振動子)。単声(v2 契約) */
typedef struct {
    int remaining;      /* 残りフレーム(0=idle) */
    float s, c;         /* sin/cos の回転状態 */
    float cw, sw;       /* 回転係数 */
    float decay, amp;
} Voice;
static Voice s_voice;

static uint64_t s_audio_samples;   /* 再生済みフレーム数(音声クロック) */
static uint32_t s_audio_epoch_ms;  /* サンプル 0 に対応する now_ms */
static bool s_audio_epoch_set;     /* エポックは最初のコールバックで確定する */
static uint32_t s_click_pending;   /* 予約時刻(0=なし) */
static ToneDef s_pending_tone;     /* 予約時のスナップショット */
static uint32_t s_click_last_fired;
static bool s_click_asap;          /* 即時発音要求: 次のバッファ先頭で発音 */
static ToneDef s_asap_tone;
static int s_master_vol = 98;      /* マスター音量(実機の既定と一致) */

/* ボイスをトーン定義から初期化(発音開始)。マスター音量は発音時に焼き込む */
static void voice_start(const ToneDef* t)
{
    const float w = 2.0f * (float)M_PI * (float)t->freq_hz / CLICK_RATE;
    s_voice.remaining = CLICK_RATE * t->dur_ms / 1000;
    s_voice.s = 0.0f;
    s_voice.c = 1.0f;
    s_voice.cw = cosf(w);
    s_voice.sw = sinf(w);
    s_voice.decay = expf(-3.5f / (float)s_voice.remaining); /* 終端で ~-30dB */
    s_voice.amp = 12000.0f * t->level / 100.0f * s_master_vol / 100.0f;
}

/* ジッタ統計: 発音開始位置(音声クロック)と壁時計を N 発ごとに集計 */
#define CLICK_STAT_N 100
static uint64_t s_fire_sample[CLICK_STAT_N];
static uint32_t s_fire_wall[CLICK_STAT_N];
static int s_fire_count;

static void click_record_fire(uint64_t sample)
{
    if (s_fire_count < CLICK_STAT_N) {
        s_fire_sample[s_fire_count] = sample;
        s_fire_wall[s_fire_count] = SDL_GetTicks() - s_start_ms;
        s_fire_count++;
    }
    if (s_fire_count == CLICK_STAT_N) {
        double smin = 1e18, smax = 0, ssum = 0;
        uint32_t wmin = UINT32_MAX, wmax = 0;
        uint64_t wsum = 0;
        for (int i = 1; i < CLICK_STAT_N; i++) {
            double ds = (double)(s_fire_sample[i] - s_fire_sample[i - 1]) * 1000.0 / CLICK_RATE;
            uint32_t dw = s_fire_wall[i] - s_fire_wall[i - 1];
            if (ds < smin) smin = ds;
            if (ds > smax) smax = ds;
            ssum += ds;
            if (dw < wmin) wmin = dw;
            if (dw > wmax) wmax = dw;
            wsum += dw;
        }
        fprintf(stderr,
                "click jitter (sample clock): min=%.3f avg=%.3f max=%.3f ms (n=%d)\n",
                smin, ssum / (CLICK_STAT_N - 1), smax, CLICK_STAT_N - 1);
        fprintf(stderr,
                "click jitter (wall clock)  : min=%u avg=%.1f max=%u ms (n=%d)\n",
                wmin, (double)wsum / (CLICK_STAT_N - 1), wmax, CLICK_STAT_N - 1);
        s_fire_count = 0;
    }
}

/* now_ms → 音声クロック上の目標フレーム */
static uint64_t click_ms_to_sample(uint32_t ms)
{
    if (ms <= s_audio_epoch_ms) return 0;
    return (uint64_t)(ms - s_audio_epoch_ms) * CLICK_RATE / 1000;
}

/* SDL オーディオスレッドから呼ばれる。stream は 16bit ステレオ */
static void audio_callback(void* userdata, Uint8* stream, int len)
{
    (void)userdata;
    memset(stream, 0, len);
    int16_t* out = (int16_t*)stream;
    const int frames = len / 4;
    const uint64_t buf_start = s_audio_samples;

    /* エポックは最初のコールバックで確定する。pull 型のコールバックは実再生より
     * バッファ深さぶん先行して呼ばれるため、これで音声クロックが壁時計より
     * わずかに先行し、「壁時計上は拍を過ぎたが未発火」の窓(アプリの毎 tick
     * 再予約が未発火の予約を置き換えて拍を落とす競合)が生じない。 */
    if (!s_audio_epoch_set) {
        s_audio_epoch_ms = SDL_GetTicks() - s_start_ms;
        s_audio_epoch_set = true;
    }

    /* 発火判定: 目標サンプルがこのバッファに入ったらオフセット付きで開始 */
    int start_off = -1;
    const ToneDef* start_tone = NULL;
    if (s_click_asap) {
        s_click_asap = false;
        start_off = 0;
        start_tone = &s_asap_tone;
        click_record_fire(buf_start);
    } else if (s_click_pending != 0 && s_click_pending > s_click_last_fired) {
        uint64_t target = click_ms_to_sample(s_click_pending);
        if (target < buf_start) target = buf_start; /* 過ぎた予約は直ちに */
        /* セーフティネット: 音声バックエンドのコールバックがバースト的に遅れて
         * サンプルクロックが壁時計より遅れた場合でも、壁時計で期限が来た予約は
         * このバッファで発音する(未発火のまま再予約に置き換えられて拍が落ちる
         * のを防ぐ)。通常はサンプル精度の経路が先に発火する。 */
        const uint32_t wall_now = SDL_GetTicks() - s_start_ms;
        const bool wall_due = (s_click_pending <= wall_now);
        if (target < buf_start + (uint64_t)frames || wall_due) {
            if (target >= buf_start + (uint64_t)frames) target = buf_start;
            start_off = (int)(target - buf_start);
            start_tone = &s_pending_tone;
            s_click_last_fired = s_click_pending;
            s_click_pending = 0;
            click_record_fire(target);
            host_midi_notify_beat_fired(s_click_last_fired); /* Phase 8b */
        }
    }

    for (int i = 0; i < frames; i++) {
        if (start_off >= 0 && i == start_off) voice_start(start_tone);
        if (s_voice.remaining > 0) {
            const float s2 = s_voice.s * s_voice.cw + s_voice.c * s_voice.sw;
            s_voice.c = s_voice.c * s_voice.cw - s_voice.s * s_voice.sw;
            s_voice.s = s2;
            s_voice.amp *= s_voice.decay;
            const int16_t v = (int16_t)(s_voice.amp * s_voice.s);
            out[i * 2] = v;
            out[i * 2 + 1] = v;
            s_voice.remaining--;
        }
    }

    s_audio_samples += (uint64_t)frames;
    /* Clock Authority(Phase 11)のレートマスター。実機の I2S on_sent と同型に、
     * 再生済みフレーム数を渡す(hostapi_seq.c が受け取る)。 */
    host_seq_on_audio((uint32_t)frames);
}

#ifdef HAVE_SDL_TTF
/* 実機(LVGL Montserrat 14, アンチエイリアス)に見た目を近づけるため、
 * TTF フォントを WINDOW_SCALE 倍のピクセルサイズでラスタライズし、
 * 論理座標では 1/WINDOW_SCALE で貼る(ウィンドウ実ピクセルで等倍=最良の AA)。
 * フォントが無い環境では font8x8 にフォールバック。 */
static TTF_Font* s_font;
#define TTF_POINT_SIZE 13

static void try_open_font(void)
{
    const char* candidates[] = {
        getenv("MIDIBOX_FONT"), /* 環境変数で差し替え可 */
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init failed: %s (falling back to font8x8)\n",
                TTF_GetError());
        return;
    }
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i]) continue;
        s_font = TTF_OpenFont(candidates[i], TTF_POINT_SIZE * WINDOW_SCALE);
        if (s_font) {
            printf("font: %s\n", candidates[i]);
            return;
        }
    }
    fprintf(stderr, "no TTF font found (falling back to font8x8)\n");
}
#endif

static TextSlot s_texts[MAX_TEXT_SLOTS];
static RectSlot s_rects[MAX_RECT_SLOTS];

/* ---- オーディオ (Phase 6B) ----
 * MP3 再生は SDL_mixer(クリック音の SDL_QueueAudio 経路とは独立のデバイス。
 * OS 側ミキサで混ざる)。状態は実機と同じ「ホスト宣言 + 自然終了の取り込み」。 */
#define MUSIC_ROOT "./sdcard/music"

static int s_audio_state = 0; /* HOSTAPI_AUDIO_* */
#ifdef HAVE_SDL_MIXER
static Mix_Music* s_music;
static volatile int s_music_finished;
static bool s_mixer_ready;

/* SDL_mixer の音楽スレッドから呼ばれる。フラグを立てるだけ */
static void music_finished_hook(void)
{
    s_music_finished = 1;
}
#endif

static void audio_refresh_finished(void)
{
#ifdef HAVE_SDL_MIXER
    if (s_audio_state == HOSTAPI_AUDIO_PLAYING && s_music_finished) {
        s_audio_state = HOSTAPI_AUDIO_FINISHED;
    }
#endif
}

void host_sdl_audio_reset(void)
{
#ifdef HAVE_SDL_MIXER
    if (s_mixer_ready) {
        Mix_HaltMusic();
        if (s_music) {
            Mix_FreeMusic(s_music);
            s_music = NULL;
        }
    }
#endif
    s_audio_state = HOSTAPI_AUDIO_STOPPED;

    /* クリック予約・last_fired・トーンパレットもリセット(Phase 7A/7C 契約)。
     * マスター音量は既定に戻す(アプリ起動時の初期状態を一定にする) */
    if (s_audio) {
        SDL_LockAudioDevice(s_audio);
        s_click_pending = 0;
        s_click_last_fired = 0;
        s_click_asap = false;
        s_voice.remaining = 0;
        s_fire_count = 0;
        s_master_vol = 98;
        for (int i = 0; i < HOSTAPI_TONE_SLOTS; i++) s_tones[i] = (ToneDef){0};
        s_tones[0] = kDefaultClick; /* slot 0 = v0 互換の既定クリック */
        SDL_UnlockAudioDevice(s_audio);
    }
    host_midi_reset(); /* MIDI Clock 生成も必ず停止する (Phase 8b 契約) */
    host_seq_reset();  /* L0/L1 も初期状態へ (Phase 11) */
}

/* ミュージックルート相対パスの検証(実機側 hostapi.cpp と同じ規則) */
static bool audio_path_ok(const char* path, uint32_t len)
{
    if (len == 0 || len > 64) return false;
    if (path[0] == '/') return false;
    for (uint32_t i = 0; i + 1 < len; i++) {
        if (path[i] == '.' && path[i + 1] == '.') return false;
    }
    return true;
}

int32_t native_hostapi_audio_play(wasm_exec_env_t exec_env, const char* path, uint32_t len)
{
    (void)exec_env;
    if (!audio_path_ok(path, len)) {
        fprintf(stderr, "audio_play: rejected path\n");
        s_audio_state = HOSTAPI_AUDIO_ERROR;
        return -1;
    }
#ifdef HAVE_SDL_MIXER
    if (!s_mixer_ready) {
        s_audio_state = HOSTAPI_AUDIO_ERROR;
        return -1;
    }
    char full[256];
    snprintf(full, sizeof(full), "%s/%.*s", MUSIC_ROOT, (int)len, path);

    Mix_HaltMusic();
    if (s_music) {
        Mix_FreeMusic(s_music);
        s_music = NULL;
    }
    s_music = Mix_LoadMUS(full);
    if (!s_music) {
        fprintf(stderr, "audio_play: %s: %s\n", full, Mix_GetError());
        s_audio_state = HOSTAPI_AUDIO_ERROR;
        return -1;
    }
    s_music_finished = 0;
    if (Mix_PlayMusic(s_music, 1) != 0) {
        fprintf(stderr, "audio_play: %s\n", Mix_GetError());
        Mix_FreeMusic(s_music);
        s_music = NULL;
        s_audio_state = HOSTAPI_AUDIO_ERROR;
        return -1;
    }
    printf("audio_play: %s\n", full);
    s_audio_state = HOSTAPI_AUDIO_PLAYING;
    return 0;
#else
    fprintf(stderr, "audio_play: built without SDL_mixer\n");
    s_audio_state = HOSTAPI_AUDIO_ERROR;
    return -1;
#endif
}

int32_t native_hostapi_audio_ctrl(wasm_exec_env_t exec_env, int32_t cmd)
{
    (void)exec_env;
    audio_refresh_finished();
    switch (cmd) {
    case HOSTAPI_AUDIO_CMD_PAUSE:
        if (s_audio_state != HOSTAPI_AUDIO_PLAYING) return -1;
#ifdef HAVE_SDL_MIXER
        Mix_PauseMusic();
#endif
        s_audio_state = HOSTAPI_AUDIO_PAUSED;
        return 0;
    case HOSTAPI_AUDIO_CMD_RESUME:
        if (s_audio_state != HOSTAPI_AUDIO_PAUSED) return -1;
#ifdef HAVE_SDL_MIXER
        Mix_ResumeMusic();
#endif
        s_audio_state = HOSTAPI_AUDIO_PLAYING;
        return 0;
    case HOSTAPI_AUDIO_CMD_STOP:
#ifdef HAVE_SDL_MIXER
        if (s_mixer_ready) Mix_HaltMusic();
#endif
        s_audio_state = HOSTAPI_AUDIO_STOPPED;
        return 0;
    default:
        return -1;
    }
}

void native_hostapi_audio_set_volume(wasm_exec_env_t exec_env, int32_t v)
{
    (void)exec_env;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    /* マスター音量 (v2): MP3 とクリックの両方に適用 */
    if (s_audio) {
        SDL_LockAudioDevice(s_audio);
        s_master_vol = v;
        SDL_UnlockAudioDevice(s_audio);
    }
#ifdef HAVE_SDL_MIXER
    if (s_mixer_ready) Mix_VolumeMusic(v * MIX_MAX_VOLUME / 100);
#endif
}

int32_t native_hostapi_audio_get_state(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    audio_refresh_finished();
    return s_audio_state;
}

/* ---- ファイル列挙 (Phase 6C) ----
 * 実機側 hostapi.cpp と同じ契約: MUSIC_ROOT 直下の .mp3 を idx で列挙 */
static bool has_mp3_ext(const char* name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".mp3") == 0;
}

int32_t native_hostapi_fs_list(wasm_exec_env_t exec_env, int32_t idx,
                               char* buf, uint32_t buf_len)
{
    (void)exec_env;
    if (idx < 0) return -1;

    DIR* dir = opendir(MUSIC_ROOT);
    if (!dir) return -1;

    int32_t found = -1;
    int32_t count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!has_mp3_ext(ent->d_name)) continue;
        size_t name_len = strlen(ent->d_name);
        if (name_len > 63) continue; /* 契約: 63 バイト超は列挙から除外 */

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", MUSIC_ROOT, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (count == idx) {
            uint32_t n = (name_len < buf_len) ? (uint32_t)name_len : buf_len;
            memcpy(buf, ent->d_name, n);
            found = (int32_t)n;
            break;
        }
        count++;
    }
    closedir(dir);
    return found;
}

/* ---- 入力イベントキュー (Phase 6A) ----
 * 実機と同じ規約: 深さ 16、満杯は最古から捨てる、DOWN 未配送の UP は捨てる。
 * Linux は main ループ単一スレッドなのでロック不要。 */
static hostapi_event_t s_evq[EVENT_QUEUE_DEPTH];
static int s_evq_head = 0;
static int s_evq_count = 0;
static bool s_down_delivered = false;

void host_sdl_clear_events(void)
{
    s_evq_head = 0;
    s_evq_count = 0;
    s_down_delivered = false;
}

void host_sdl_push_touch(bool down, int x, int y)
{
    /* アプリを起動したクリックの UP がアプリに漏れないように */
    if (!down && !s_down_delivered) return;
    if (down) s_down_delivered = true;

    if (s_evq_count == EVENT_QUEUE_DEPTH) { /* 満杯: 最古を捨てる */
        s_evq_head = (s_evq_head + 1) % EVENT_QUEUE_DEPTH;
        s_evq_count--;
        fprintf(stderr, "event queue full, dropped oldest\n");
    }
    hostapi_event_t* ev = &s_evq[(s_evq_head + s_evq_count) % EVENT_QUEUE_DEPTH];
    ev->type = down ? HOSTAPI_EV_TOUCH_DOWN : HOSTAPI_EV_TOUCH_UP;
    ev->param = 0;
    ev->x = (int16_t)x;
    ev->y = (int16_t)y;
    ev->time_ms = SDL_GetTicks() - s_start_ms;
    s_evq_count++;
}

bool host_sdl_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    /* ALLOW_HIGHDPI: ディスプレイスケール環境(ChromeOS 等)でウィンドウサイズと
     * マウスイベントの単位(ポイント)を一致させる。無いとイベントだけ 1/scale に
     * なりヒットテストがずれる */
    s_window = SDL_CreateWindow("MidiAppBox WASM host",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                SCREEN_W * WINDOW_SCALE, SCREEN_H * WINDOW_SCALE,
                                SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer) {
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_RenderSetLogicalSize(s_renderer, SCREEN_W, SCREEN_H);

#ifdef HAVE_SDL_TTF
    try_open_font();
#endif

    s_start_ms = SDL_GetTicks();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = CLICK_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024; /* コールバック粒度 ~23ms(発音自体はバッファ内オフセットでサンプル精度) */
    want.callback = audio_callback;
    s_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_audio) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s (continuing without sound)\n",
                SDL_GetError());
    } else {
        SDL_PauseAudioDevice(s_audio, 0);
    }

#ifdef HAVE_SDL_MIXER
    if ((Mix_Init(MIX_INIT_MP3) & MIX_INIT_MP3) == 0) {
        /* click デバイスとは独立(OS ミキサで混合) */
        fprintf(stderr, "Mix_Init(MP3) failed: %s (audio_play disabled)\n",
                Mix_GetError());
    } else if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
        fprintf(stderr, "Mix_OpenAudio failed: %s (audio_play disabled)\n",
                Mix_GetError());
    } else {
        s_mixer_ready = true;
        Mix_HookMusicFinished(music_finished_hook);
        Mix_VolumeMusic(98 * MIX_MAX_VOLUME / 100); /* 実機の既定音量 98 に合わせる */
    }
#endif

    return true;
}

void host_sdl_shutdown(void)
{
#ifdef HAVE_SDL_TTF
    if (s_font) TTF_CloseFont(s_font);
    if (TTF_WasInit()) TTF_Quit();
#endif
#ifdef HAVE_SDL_MIXER
    host_sdl_audio_reset();
    if (s_mixer_ready) Mix_CloseAudio();
    Mix_Quit();
#endif
    if (s_audio) SDL_CloseAudioDevice(s_audio);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window) SDL_DestroyWindow(s_window);
    SDL_Quit();
}

static void draw_char8x8(int32_t x, int32_t y, unsigned char c)
{
    if (c >= 128) c = '?';
    const char* glyph = font8x8_basic[c];
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            if (glyph[row] & (1 << col)) {
                SDL_RenderDrawPoint(s_renderer, x + col, y + row);
            }
        }
    }
}

/* テキスト描画の共通経路。TTF があればアンチエイリアス描画、無ければ font8x8 */
static void draw_string(int x, int y, const char* s, uint32_t rgb888)
{
#ifdef HAVE_SDL_TTF
    if (s_font && s[0]) {
        SDL_Color color = { (Uint8)(rgb888 >> 16), (Uint8)(rgb888 >> 8),
                            (Uint8)rgb888, 255 };
        SDL_Surface* surf = TTF_RenderUTF8_Blended(s_font, s, color);
        if (surf) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(s_renderer, surf);
            if (tex) {
                SDL_Rect dst = { x, y, surf->w / WINDOW_SCALE,
                                 surf->h / WINDOW_SCALE };
                SDL_RenderCopy(s_renderer, tex, NULL, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_FreeSurface(surf);
            return;
        }
    }
#endif
    SDL_SetRenderDrawColor(s_renderer, (rgb888 >> 16) & 0xff, (rgb888 >> 8) & 0xff,
                           rgb888 & 0xff, 255);
    for (size_t k = 0; s[k]; ++k) {
        draw_char8x8(x + (int)k * 8, y, (unsigned char)s[k]);
    }
}

/* ---- 直描画ヘルパ(ランチャーメニュー用。retained スロットとは別系統) ---- */

void host_sdl_clear_slots(void)
{
    memset(s_texts, 0, sizeof(s_texts));
    memset(s_rects, 0, sizeof(s_rects));
}

void host_sdl_begin_frame(uint32_t rgb888)
{
    SDL_SetRenderDrawColor(s_renderer, (rgb888 >> 16) & 0xff, (rgb888 >> 8) & 0xff,
                           rgb888 & 0xff, 255);
    SDL_RenderClear(s_renderer);
}

void host_sdl_rect(int x, int y, int w, int h, uint32_t rgb888)
{
    SDL_SetRenderDrawColor(s_renderer, (rgb888 >> 16) & 0xff, (rgb888 >> 8) & 0xff,
                           rgb888 & 0xff, 255);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(s_renderer, &rect);
}

void host_sdl_text(int x, int y, const char* s, uint32_t rgb888)
{
    draw_string(x, y, s, rgb888);
}

void host_sdl_present(void)
{
    SDL_RenderPresent(s_renderer);
}

void host_sdl_debug_dump_coords(int wx, int wy, int lx, int ly)
{
    int ww = 0, wh = 0, ow = 0, oh = 0;
    float sx = 1, sy = 1;
    SDL_Rect vp;
    SDL_GetWindowSize(s_window, &ww, &wh);
    SDL_GetRendererOutputSize(s_renderer, &ow, &oh);
    SDL_RenderGetScale(s_renderer, &sx, &sy);
    SDL_RenderGetViewport(s_renderer, &vp);
    printf("click: ev=(%d,%d) win=(%d,%d) out=(%d,%d) scale=(%.2f,%.2f) "
           "vp=(%d,%d,%d,%d) -> logical=(%d,%d)\n",
           wx, wy, ww, wh, ow, oh, sx, sy, vp.x, vp.y, vp.w, vp.h, lx, ly);
    fflush(stdout);
}

void host_sdl_window_to_logical(int wx, int wy, int* lx, int* ly)
{
    /* SDL2 は SDL_RenderSetLogicalSize を設定すると、マウスイベント座標を
     * 自動で論理座標(SCREEN_W x SCREEN_H)へ変換して届ける
     * (SDL_RendererEventWatch)。したがってここでは変換せず、クランプのみ行う。
     * 追加で割ると二重変換になり、ヒットテストが左上 1/4 に縮む。 */
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;
    if (wx >= SCREEN_W) wx = SCREEN_W - 1;
    if (wy >= SCREEN_H) wy = SCREEN_H - 1;
    *lx = wx;
    *ly = wy;
}

void host_sdl_render(void)
{
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);

    for (int i = 0; i < MAX_RECT_SLOTS; ++i) {
        if (!s_rects[i].used) continue;
        const RectSlot* r = &s_rects[i];
        SDL_SetRenderDrawColor(s_renderer, (r->rgb888 >> 16) & 0xff,
                               (r->rgb888 >> 8) & 0xff, r->rgb888 & 0xff, 255);
        SDL_Rect rect = { r->x, r->y, r->w, r->h };
        SDL_RenderFillRect(s_renderer, &rect);
    }

    for (int i = 0; i < MAX_TEXT_SLOTS; ++i) {
        if (!s_texts[i].used) continue;
        const TextSlot* t = &s_texts[i];
        draw_string(t->x, t->y, t->text, 0xffffff);
    }

    SDL_RenderPresent(s_renderer);
}

/* ---- natives (wasm import "env") ---- */

void native_hostapi_draw_text(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                              const char* str, uint32_t len)
{
    (void)exec_env;
    TextSlot* slot = NULL;
    for (int i = 0; i < MAX_TEXT_SLOTS; ++i) {
        if (s_texts[i].used && s_texts[i].x == x && s_texts[i].y == y) {
            slot = &s_texts[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < MAX_TEXT_SLOTS; ++i) {
            if (!s_texts[i].used) { slot = &s_texts[i]; break; }
        }
    }
    if (!slot) {
        fprintf(stderr, "draw_text: no free slot (max %d)\n", MAX_TEXT_SLOTS);
        return;
    }
    if (len > MAX_TEXT_LEN) len = MAX_TEXT_LEN;
    memcpy(slot->text, str, len);
    slot->text[len] = '\0';
    slot->x = x;
    slot->y = y;
    slot->used = true;
}

void native_hostapi_fill_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint32_t rgb888)
{
    (void)exec_env;
    RectSlot* slot = NULL;
    for (int i = 0; i < MAX_RECT_SLOTS; ++i) {
        if (s_rects[i].used && s_rects[i].x == x && s_rects[i].y == y) {
            slot = &s_rects[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < MAX_RECT_SLOTS; ++i) {
            if (!s_rects[i].used) { slot = &s_rects[i]; break; }
        }
    }
    if (!slot) {
        fprintf(stderr, "fill_rect: no free slot (max %d)\n", MAX_RECT_SLOTS);
        return;
    }
    slot->x = x;
    slot->y = y;
    slot->w = w;
    slot->h = h;
    slot->rgb888 = rgb888;
    slot->used = true;
}

/* slot を解決してコピーを返す(未定義なら false)。ロック外から呼ぶこと */
static bool tone_lookup(int32_t slot, ToneDef* out)
{
    if (slot < 0 || slot >= HOSTAPI_TONE_SLOTS) return false;
    bool ok;
    SDL_LockAudioDevice(s_audio);
    ok = s_tones[slot].defined;
    if (ok) *out = s_tones[slot];
    SDL_UnlockAudioDevice(s_audio);
    return ok;
}

static int32_t tone_play_impl(int32_t slot)
{
    if (!s_audio) return -1;
    ToneDef tone;
    if (!tone_lookup(slot, &tone)) return -1;
    /* 即時発音 = 次のコールバックバッファ先頭で開始 */
    SDL_LockAudioDevice(s_audio);
    s_asap_tone = tone;
    s_click_asap = true;
    SDL_UnlockAudioDevice(s_audio);
    return 0;
}

/* L0 の CLICK ポート(Phase 11)からの発音。既存の即時発音経路
 * (tone_play_impl → s_click_asap → audio_callback)をそのまま共有の出口に
 * するので、音声側の追加配線はない。 */
void host_click_play_slot(uint32_t slot)
{
    (void)tone_play_impl((int32_t)slot);
}

static int32_t tone_schedule_impl(int32_t slot, int32_t time_ms)
{
    if (!s_audio) return -1;
    const uint32_t t = (uint32_t)time_ms;
    const uint32_t now = SDL_GetTicks() - s_start_ms;

    if (t == 0) { /* キャンセル(slot によらず有効) */
        SDL_LockAudioDevice(s_audio);
        s_click_pending = 0;
        SDL_UnlockAudioDevice(s_audio);
        return 0;
    }

    ToneDef tone;
    if (!tone_lookup(slot, &tone)) return -1;

    bool scheduled = false;
    bool fire_old = false;
    uint32_t last_fired_snapshot = 0;
    SDL_LockAudioDevice(s_audio);
    if (t > s_click_last_fired) {
        /* 置き換えガード: 期限到来済みの未発火予約を破棄しない。
         * 先にその予約を「可及的速やか」に発音扱いにしてから置き換える */
        if (s_click_pending != 0 && s_click_pending != t &&
            s_click_pending <= now && s_click_pending > s_click_last_fired) {
            s_click_last_fired = s_click_pending;
            s_asap_tone = s_pending_tone;
            s_click_asap = true;
            fire_old = true;
        }
        s_click_pending = t; /* 置き換え予約(last_fired 以前は無視) */
        s_pending_tone = tone; /* 予約時スナップショット */
        scheduled = true;
        last_fired_snapshot = s_click_last_fired;
    }
    SDL_UnlockAudioDevice(s_audio);
    if (scheduled) {
        /* Phase 8b: 新しい予約(t)が確定した時点でテンポを staging する。
         * fire_old で旧予約を発音扱いにする場合は、その通知より先に行う
         * (旧拍の発音通知が最新テンポを picks up できるように)。 */
        host_midi_notify_beat_scheduled(t);
        if (fire_old) {
            host_midi_notify_beat_fired(last_fired_snapshot);
        }
    }
    return 0;
}

void native_hostapi_play_click(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    tone_play_impl(0);
}

int32_t native_hostapi_click_schedule(wasm_exec_env_t exec_env, int32_t time_ms)
{
    (void)exec_env;
    return tone_schedule_impl(0, time_ms);
}

int32_t native_hostapi_tone_define(wasm_exec_env_t exec_env, int32_t slot,
                                   int32_t wave, int32_t freq_hz, int32_t dur_ms,
                                   int32_t level)
{
    (void)exec_env;
    if (slot < 0 || slot >= HOSTAPI_TONE_SLOTS) return -1;
    if (wave != HOSTAPI_WAVE_SINE) return -1; /* 未知の波形(トラップしない) */
    if (!s_audio) return -1;

    if (freq_hz < 100) freq_hz = 100;
    if (freq_hz > 8000) freq_hz = 8000;
    if (dur_ms < 5) dur_ms = 5;
    if (dur_ms > 100) dur_ms = 100;
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    SDL_LockAudioDevice(s_audio);
    s_tones[slot] = (ToneDef){true, (uint16_t)freq_hz, (uint16_t)dur_ms, (uint8_t)level};
    SDL_UnlockAudioDevice(s_audio);
    return 0;
}

int32_t native_hostapi_tone_play(wasm_exec_env_t exec_env, int32_t slot)
{
    (void)exec_env;
    return tone_play_impl(slot);
}

int32_t native_hostapi_tone_schedule(wasm_exec_env_t exec_env, int32_t slot,
                                     int32_t time_ms)
{
    (void)exec_env;
    return tone_schedule_impl(slot, time_ms);
}

uint32_t native_hostapi_now_ms(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    /* SDL_GetTicks は内部で CLOCK_MONOTONIC 相当。起動からの経過 ms を返す */
    return SDL_GetTicks() - s_start_ms;
}

/* buf は WAMR 境界検証済み(シグネチャ "*~")。書いた件数を返す */
int32_t native_hostapi_poll_event(wasm_exec_env_t exec_env, char* buf, uint32_t len)
{
    (void)exec_env;
    const uint32_t max_events = len / sizeof(hostapi_event_t);
    int32_t n = 0;
    while (n < (int32_t)max_events && s_evq_count > 0) {
        memcpy(buf + n * sizeof(hostapi_event_t), &s_evq[s_evq_head],
               sizeof(hostapi_event_t));
        s_evq_head = (s_evq_head + 1) % EVENT_QUEUE_DEPTH;
        s_evq_count--;
        n++;
    }
    return n;
}
