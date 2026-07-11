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

#include "font8x8_basic.h"

/* 実機と同じランドスケープ 320x240 */
#define SCREEN_W 320
#define SCREEN_H 240
#define WINDOW_SCALE 2

#define MAX_TEXT_SLOTS 16
#define MAX_RECT_SLOTS 16
#define MAX_TEXT_LEN 63

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

static TextSlot s_texts[MAX_TEXT_SLOTS];
static RectSlot s_rects[MAX_RECT_SLOTS];

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

    s_start_ms = SDL_GetTicks();
    return true;
}

void host_sdl_shutdown(void)
{
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
    SDL_SetRenderDrawColor(s_renderer, (rgb888 >> 16) & 0xff, (rgb888 >> 8) & 0xff,
                           rgb888 & 0xff, 255);
    for (size_t k = 0; s[k]; ++k) {
        draw_char8x8(x + (int)k * 8, y, (unsigned char)s[k]);
    }
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

    SDL_SetRenderDrawColor(s_renderer, 255, 255, 255, 255);
    for (int i = 0; i < MAX_TEXT_SLOTS; ++i) {
        if (!s_texts[i].used) continue;
        const TextSlot* t = &s_texts[i];
        for (size_t k = 0; t->text[k]; ++k) {
            draw_char8x8(t->x + (int32_t)k * 8, t->y, (unsigned char)t->text[k]);
        }
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
