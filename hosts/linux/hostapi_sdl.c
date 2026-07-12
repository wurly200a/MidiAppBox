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

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    s_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_audio) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s (continuing without sound)\n",
                SDL_GetError());
    } else {
        SDL_PauseAudioDevice(s_audio, 0);
    }

#ifdef HAVE_SDL_MIXER
    if ((Mix_Init(MIX_INIT_MP3) & MIX_INIT_MP3) == 0) {
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

    s_start_ms = SDL_GetTicks();
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

void native_hostapi_play_click(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_audio) return;

    /* 実機 (audio::Mp3Player::play_click) と同じ波形: 1kHz 減衰サイン 30ms */
    enum { kSampleRate = 44100, kFrames = kSampleRate * 30 / 1000 };
    static int16_t buf[kFrames * 2];
    static bool generated = false;
    if (!generated) {
        const float kFreq = 1000.0f;
        const float kAmp = 12000.0f;
        for (int i = 0; i < kFrames; ++i) {
            float t = (float)i / kSampleRate;
            float decay = expf(-t * 120.0f);
            int16_t s = (int16_t)lroundf(kAmp * decay * sinf(2.0f * (float)M_PI * kFreq * t));
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        generated = true;
    }
    SDL_QueueAudio(s_audio, buf, sizeof(buf));
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
