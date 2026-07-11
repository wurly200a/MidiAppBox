/*
 * MidiAppBox WASM PoC — Linux ホスト(ランチャー付き)。
 *
 * 実機と同じ構成: WAMR (fast interpreter, Alloc_With_Pool 64KB) を常駐させ、
 * 共通のホスト API 定義 (shared/hostapi_defs.h) で .wasm を実行する。
 * ライフサイクルも実機と同一: load → app_init → 100ms tick →
 * (export されていれば) app_exit → exec_env → instance → module の順に破棄。
 *
 * 使い方:
 *   midibox_host                 ... ../../wasm-apps をスキャンしてメニュー表示
 *   midibox_host <dir>           ... 指定ディレクトリをスキャンしてメニュー表示
 *   midibox_host <file.wasm>     ... 単発実行(メニューなし。CI スモーク用)
 *
 * 操作: マウスクリックで起動 / ESC でメニューに戻る(実機の power_key 短押し相当)
 *       メニューで ESC またはウィンドウクローズで終了
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include <SDL.h>

#include "wasm_export.h"
#include "hostapi_defs.h"
#include "hostapi_sdl.h"

#define APP_TICK_MS 100
#define MAX_APPS 32

/* メニューレイアウト(320x240 論理座標) */
#define MENU_ROW_X 10
#define MENU_ROW_W 300
#define MENU_ROW_Y0 32
#define MENU_ROW_H 20
#define MENU_ROW_GAP 2
#define MENU_STATUS_Y 220

static uint8_t s_wamr_heap[64 * 1024];

static NativeSymbol s_native_symbols[] = {
    HOSTAPI_NATIVE_SYMBOLS(HOSTAPI_SYMBOL_ENTRY)
};

typedef struct {
    char name[64];
    char path[512];
} AppEntry;

static AppEntry s_apps[MAX_APPS];
static int s_app_count = 0;
static char s_status[128] = "";
static char s_apps_dir[384] = "";

/* ---- アプリ一覧スキャン(dir 直下と 1 段下のサブディレクトリ) ---- */

static void add_app(const char* name, const char* path)
{
    if (s_app_count >= MAX_APPS) return;
    snprintf(s_apps[s_app_count].name, sizeof(s_apps[0].name), "%s", name);
    snprintf(s_apps[s_app_count].path, sizeof(s_apps[0].path), "%s", path);
    s_app_count++;
}

static bool has_wasm_ext(const char* name)
{
    size_t len = strlen(name);
    return len > 5 && strcasecmp(name + len - 5, ".wasm") == 0;
}

static int cmp_app(const void* a, const void* b)
{
    return strcmp(((const AppEntry*)a)->name, ((const AppEntry*)b)->name);
}

static void scan_apps(const char* dir)
{
    s_app_count = 0;
    DIR* d = opendir(dir);
    if (!d) {
        snprintf(s_status, sizeof(s_status), "cannot open %s", dir);
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISREG(st.st_mode) && has_wasm_ext(ent->d_name)) {
            add_app(ent->d_name, path);
        } else if (S_ISDIR(st.st_mode)) {
            /* リポジトリの wasm-apps/<app>/<app>.wasm 配置にも対応 */
            DIR* sub = opendir(path);
            if (!sub) continue;
            struct dirent* se;
            while ((se = readdir(sub)) != NULL) {
                if (!has_wasm_ext(se->d_name)) continue;
                char spath[512];
                int n = snprintf(spath, sizeof(spath), "%s/%s", path, se->d_name);
                if (n > 0 && (size_t)n < sizeof(spath)) add_app(se->d_name, spath);
            }
            closedir(sub);
        }
    }
    closedir(d);
    qsort(s_apps, s_app_count, sizeof(AppEntry), cmp_app);

    if (s_app_count == 0) {
        snprintf(s_status, sizeof(s_status), "no .wasm found in %s", dir);
    } else {
        snprintf(s_status, sizeof(s_status), "%d app(s) - click to launch", s_app_count);
    }
}

/* ---- アプリのロード/破棄(実機 wasm_runtime.cpp の app_thread に対応) ---- */

typedef struct {
    uint8_t* buf;
    wasm_module_t module;
    wasm_module_inst_t inst;
    wasm_exec_env_t exec_env;
    wasm_function_inst_t fn_tick;
    wasm_function_inst_t fn_exit;
} App;

static uint8_t* read_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(s_status, sizeof(s_status), "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 512 * 1024) {
        snprintf(s_status, sizeof(s_status), "bad file size (%ld)", size);
        fclose(f);
        return NULL;
    }
    uint8_t* buf = malloc(size);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        snprintf(s_status, sizeof(s_status), "read failed: %s", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (uint32_t)size;
    return buf;
}

/* 成功で true。失敗時は s_status にエラーを入れ、途中生成物は破棄する */
static bool app_load(const char* path, App* a)
{
    char error_buf[128];
    memset(a, 0, sizeof(*a));

    uint32_t size = 0;
    /* interpreter はバッファを module 生存中参照するので unload まで保持する */
    a->buf = read_file(path, &size);
    if (!a->buf) return false;

    a->module = wasm_runtime_load(a->buf, size, error_buf, sizeof(error_buf));
    if (!a->module) {
        snprintf(s_status, sizeof(s_status), "load: %s", error_buf);
        goto fail;
    }
    a->inst = wasm_runtime_instantiate(a->module, 8 * 1024, 8 * 1024,
                                       error_buf, sizeof(error_buf));
    if (!a->inst) {
        snprintf(s_status, sizeof(s_status), "instantiate: %s", error_buf);
        goto fail;
    }
    a->exec_env = wasm_runtime_create_exec_env(a->inst, 8 * 1024);
    if (!a->exec_env) {
        snprintf(s_status, sizeof(s_status), "create_exec_env failed");
        goto fail;
    }

    {
        wasm_function_inst_t fn_init = wasm_runtime_lookup_function(a->inst, "app_init");
        a->fn_tick = wasm_runtime_lookup_function(a->inst, "app_tick");
        a->fn_exit = wasm_runtime_lookup_function(a->inst, "app_exit");
        if (!fn_init || !a->fn_tick) {
            snprintf(s_status, sizeof(s_status), "app_init/app_tick not exported");
            goto fail;
        }

        host_sdl_clear_slots(); /* 実機のアプリスクリーン再生成に相当 */
        uint32_t argv[1] = {0};
        if (!wasm_runtime_call_wasm(a->exec_env, fn_init, 0, argv)) {
            snprintf(s_status, sizeof(s_status), "app_init: %s",
                     wasm_runtime_get_exception(a->inst));
            goto fail;
        }
        printf("app started: %s (app_init=%d)\n", path, (int)argv[0]);
    }
    return true;

fail:
    if (a->exec_env) wasm_runtime_destroy_exec_env(a->exec_env);
    if (a->inst) wasm_runtime_deinstantiate(a->inst);
    if (a->module) wasm_runtime_unload(a->module);
    free(a->buf);
    memset(a, 0, sizeof(*a));
    return false;
}

static void app_unload(App* a, bool clean_stop)
{
    if (clean_stop && a->fn_exit) {
        if (!wasm_runtime_call_wasm(a->exec_env, a->fn_exit, 0, NULL)) {
            fprintf(stderr, "app_exit trapped: %s\n",
                    wasm_runtime_get_exception(a->inst));
        }
    }
    /* 破棄は必ずこの順序: exec_env → instance → module → wasm バッファ */
    if (a->exec_env) wasm_runtime_destroy_exec_env(a->exec_env);
    if (a->inst) wasm_runtime_deinstantiate(a->inst);
    if (a->module) wasm_runtime_unload(a->module);
    free(a->buf);
    memset(a, 0, sizeof(*a));
    host_sdl_clear_slots();
    printf("app stopped\n");
}

/* ---- メニュー描画とヒットテスト ---- */

static void menu_render(int hover)
{
    char title[420];
    snprintf(title, sizeof(title), "WASM Apps (%s)", s_apps_dir);
    title[38] = '\0'; /* 320px / 8px = 40 文字まで */

    host_sdl_begin_frame(0x101418);
    host_sdl_text(10, 10, title, 0xffffff);

    for (int i = 0; i < s_app_count; i++) {
        const int y = MENU_ROW_Y0 + i * (MENU_ROW_H + MENU_ROW_GAP);
        if (y + MENU_ROW_H > MENU_STATUS_Y) break; /* あふれは表示しない(PoC) */
        host_sdl_rect(MENU_ROW_X, y, MENU_ROW_W, MENU_ROW_H,
                      (i == hover) ? 0x3a4a60 : 0x2a3340);
        host_sdl_text(MENU_ROW_X + 8, y + 2, s_apps[i].name, 0xffffff);
    }

    host_sdl_text(10, MENU_STATUS_Y, s_status, 0x90a0b0);
    host_sdl_present();
}

static int menu_hit_test(int lx, int ly)
{
    if (lx < MENU_ROW_X || lx >= MENU_ROW_X + MENU_ROW_W) return -1;
    for (int i = 0; i < s_app_count; i++) {
        const int y = MENU_ROW_Y0 + i * (MENU_ROW_H + MENU_ROW_GAP);
        if (ly >= y && ly < y + MENU_ROW_H) return i;
    }
    return -1;
}

/* ---- main ---- */

int main(int argc, char** argv)
{
    bool single_mode = false;
    const char* single_path = NULL;

    if (argc > 1 && has_wasm_ext(argv[1])) {
        single_mode = true;
        single_path = argv[1];
    } else {
        snprintf(s_apps_dir, sizeof(s_apps_dir), "%s",
                 (argc > 1) ? argv[1] : "../../wasm-apps");
    }

    if (!host_sdl_init()) return 1;

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = s_wamr_heap;
    init_args.mem_alloc_option.pool.heap_size = sizeof(s_wamr_heap);

    int ret = 1;
    if (!wasm_runtime_full_init(&init_args)) {
        fprintf(stderr, "wasm_runtime_full_init failed\n");
        goto out;
    }
    if (!wasm_runtime_register_natives(
            "env", s_native_symbols,
            sizeof(s_native_symbols) / sizeof(s_native_symbols[0]))) {
        fprintf(stderr, "register_natives failed\n");
        goto out;
    }

    {
        App app;
        bool app_running = false;
        int hover = -1;
        bool quit = false;

        if (single_mode) {
            if (!app_load(single_path, &app)) {
                fprintf(stderr, "%s\n", s_status);
                goto out;
            }
            app_running = true;
            printf("single mode: close window or press ESC to quit\n");
        } else {
            scan_apps(s_apps_dir);
            printf("launcher: %s (click to launch, ESC to return)\n", s_apps_dir);
        }

        while (!quit) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    quit = true;
                } else if (ev.type == SDL_KEYDOWN &&
                           ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (app_running) {
                        /* 実機の power_key 短押し相当 */
                        app_unload(&app, true);
                        app_running = false;
                        if (single_mode) {
                            quit = true;
                        } else {
                            scan_apps(s_apps_dir);
                            snprintf(s_status, sizeof(s_status), "app stopped");
                        }
                    } else {
                        quit = true; /* メニューで ESC = 終了 */
                    }
                } else if (!app_running && !single_mode &&
                           ev.type == SDL_MOUSEMOTION) {
                    int lx, ly;
                    host_sdl_window_to_logical(ev.motion.x, ev.motion.y, &lx, &ly);
                    hover = menu_hit_test(lx, ly);
                } else if (!app_running && !single_mode &&
                           ev.type == SDL_MOUSEBUTTONDOWN &&
                           ev.button.button == SDL_BUTTON_LEFT) {
                    int lx, ly;
                    host_sdl_window_to_logical(ev.button.x, ev.button.y, &lx, &ly);
                    int idx = menu_hit_test(lx, ly);
                    if (idx >= 0) {
                        if (app_load(s_apps[idx].path, &app)) {
                            app_running = true;
                        }
                        /* 失敗時は s_status にエラーが入りメニューに留まる */
                    }
                }
            }
            if (quit) break;

            if (app_running) {
                if (!wasm_runtime_call_wasm(app.exec_env, app.fn_tick, 0, NULL)) {
                    snprintf(s_status, sizeof(s_status), "app_tick: %s",
                             wasm_runtime_get_exception(app.inst));
                    fprintf(stderr, "%s\n", s_status);
                    app_unload(&app, false);
                    app_running = false;
                    if (single_mode) {
                        quit = true;
                    } else {
                        scan_apps(s_apps_dir);
                    }
                    continue;
                }
                host_sdl_render();
                SDL_Delay(APP_TICK_MS);
            } else {
                menu_render(hover);
                SDL_Delay(30);
            }
        }

        if (app_running) app_unload(&app, true);
        ret = 0;
    }

out:
    wasm_runtime_destroy();
    host_sdl_shutdown();
    return ret;
}
