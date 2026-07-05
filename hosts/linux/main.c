/*
 * MidiAppBox WASM PoC — Linux 最小ホスト。
 *
 * 実機と同一の .wasm(既定: wasm-apps/demo/demo.wasm)を、実機と同一構成の
 * WAMR (fast interpreter, Alloc_With_Pool 128KB) + 共通のホスト API 定義
 * (shared/hostapi_defs.h) で実行する。
 *
 * 実機のホスト側 tick タスクに合わせ、100ms 周期で app_tick() を呼ぶ。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "wasm_export.h"
#include "hostapi_defs.h"
#include "hostapi_sdl.h"

static uint8_t s_wamr_heap[128 * 1024];

static NativeSymbol s_native_symbols[] = {
    HOSTAPI_NATIVE_SYMBOLS(HOSTAPI_SYMBOL_ENTRY)
};

static uint8_t* read_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(size);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (uint32_t)size;
    return buf;
}

int main(int argc, char** argv)
{
    const char* wasm_path = (argc > 1) ? argv[1] : "../../wasm-apps/demo/demo.wasm";
    int ret = 1;

    uint32_t wasm_size = 0;
    uint8_t* wasm_buf = read_file(wasm_path, &wasm_size);
    if (!wasm_buf) return 1;
    printf("loaded %s (%u bytes)\n", wasm_path, wasm_size);

    if (!host_sdl_init()) {
        free(wasm_buf);
        return 1;
    }

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = s_wamr_heap;
    init_args.mem_alloc_option.pool.heap_size = sizeof(s_wamr_heap);

    wasm_module_t module = NULL;
    wasm_module_inst_t inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    char error_buf[128];

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

    /* interpreter はバッファを module 生存中参照するので wasm_buf は保持し続ける */
    module = wasm_runtime_load(wasm_buf, wasm_size, error_buf, sizeof(error_buf));
    if (!module) {
        fprintf(stderr, "load failed: %s\n", error_buf);
        goto out;
    }
    inst = wasm_runtime_instantiate(module, 8 * 1024, 8 * 1024,
                                    error_buf, sizeof(error_buf));
    if (!inst) {
        fprintf(stderr, "instantiate failed: %s\n", error_buf);
        goto out;
    }
    exec_env = wasm_runtime_create_exec_env(inst, 8 * 1024);
    if (!exec_env) {
        fprintf(stderr, "create_exec_env failed\n");
        goto out;
    }

    {
        wasm_function_inst_t fn_init = wasm_runtime_lookup_function(inst, "app_init");
        wasm_function_inst_t fn_tick = wasm_runtime_lookup_function(inst, "app_tick");
        if (!fn_init || !fn_tick) {
            fprintf(stderr, "app_init/app_tick not exported\n");
            goto out;
        }

        uint32_t argv_buf[1] = {0};
        if (!wasm_runtime_call_wasm(exec_env, fn_init, 0, argv_buf)) {
            fprintf(stderr, "app_init failed: %s\n", wasm_runtime_get_exception(inst));
            goto out;
        }
        printf("app_init() = %d, tick loop start (close window to quit)\n",
               (int)argv_buf[0]);

        bool running = true;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
            }
            if (!wasm_runtime_call_wasm(exec_env, fn_tick, 0, NULL)) {
                fprintf(stderr, "app_tick trapped: %s\n",
                        wasm_runtime_get_exception(inst));
                goto out;
            }
            host_sdl_render();
            SDL_Delay(100);
        }
        ret = 0;
    }

out:
    if (exec_env) wasm_runtime_destroy_exec_env(exec_env);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
    wasm_runtime_destroy();
    free(wasm_buf);
    host_sdl_shutdown();
    return ret;
}
