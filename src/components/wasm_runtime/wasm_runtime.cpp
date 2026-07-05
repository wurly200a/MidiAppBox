#include "wasm_runtime.hpp"
#include "hostapi.hpp"

#include "wasm_export.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <pthread.h>
#include <cstring>

static const char* TAG = "WASM";

// EMBED_FILES で埋め込んだ .wasm
extern const uint8_t hello_wasm_start[] asm("_binary_hello_wasm_start");
extern const uint8_t hello_wasm_end[]   asm("_binary_hello_wasm_end");
extern const uint8_t demo_wasm_start[] asm("_binary_demo_wasm_start");
extern const uint8_t demo_wasm_end[]   asm("_binary_demo_wasm_end");

// WAMR グローバルヒーププール(内部 SRAM, BSS)
static uint8_t s_wamr_heap[128 * 1024];

namespace wasmrt {

bool run_selftest()
{
    const size_t heap_before = esp_get_free_heap_size();
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free heap before init: %u (internal %u)",
             (unsigned)heap_before, (unsigned)internal_before);

    const int64_t t0 = esp_timer_get_time();

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = s_wamr_heap;
    init_args.mem_alloc_option.pool.heap_size = sizeof(s_wamr_heap);

    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "wasm_runtime_full_init failed");
        return false;
    }

    bool ok = false;
    wasm_module_t module = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t exec_env = nullptr;
    uint8_t* wasm_buf = nullptr;
    char error_buf[128];

    const uint32_t wasm_size = (uint32_t)(hello_wasm_end - hello_wasm_start);
    ESP_LOGI(TAG, "loading hello.wasm (%u bytes)", (unsigned)wasm_size);

    do {
        // interpreter モードの WAMR は load 後もバッファを参照し続け、
        // fast-interp はバッファを書き換えるため、可変コピーを unload まで保持する
        wasm_buf = (uint8_t*)malloc(wasm_size);
        if (!wasm_buf) {
            ESP_LOGE(TAG, "malloc for wasm buf failed");
            break;
        }
        memcpy(wasm_buf, hello_wasm_start, wasm_size);

        module = wasm_runtime_load(wasm_buf, wasm_size, error_buf, sizeof(error_buf));
        if (!module) {
            ESP_LOGE(TAG, "load failed: %s", error_buf);
            break;
        }

        inst = wasm_runtime_instantiate(module, 8 * 1024 /*stack*/, 8 * 1024 /*heap*/,
                                        error_buf, sizeof(error_buf));
        if (!inst) {
            ESP_LOGE(TAG, "instantiate failed: %s", error_buf);
            break;
        }

        exec_env = wasm_runtime_create_exec_env(inst, 8 * 1024);
        if (!exec_env) {
            ESP_LOGE(TAG, "create_exec_env failed");
            break;
        }

        const int32_t export_count = wasm_runtime_get_export_count(module);
        for (int32_t i = 0; i < export_count; i++) {
            wasm_export_t ex;
            wasm_runtime_get_export_type(module, i, &ex);
            ESP_LOGI(TAG, "export[%d]: kind=%d name='%s'", (int)i, (int)ex.kind, ex.name);
        }

        wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "app_init");
        if (!fn) {
            ESP_LOGE(TAG, "app_init not found in module");
            break;
        }

        uint32_t argv[1] = {0};
        if (!wasm_runtime_call_wasm(exec_env, fn, 0, argv)) {
            ESP_LOGE(TAG, "call failed: %s", wasm_runtime_get_exception(inst));
            break;
        }

        const int64_t t1 = esp_timer_get_time();
        ESP_LOGI(TAG, "app_init() returned %d (init+load+call took %lld us)",
                 (int)argv[0], (long long)(t1 - t0));
        ok = ((int)argv[0] == 42);
    } while (false);

    const size_t heap_loaded = esp_get_free_heap_size();
    ESP_LOGI(TAG, "free heap with module loaded: %u (delta %d)",
             (unsigned)heap_loaded, (int)(heap_before - heap_loaded));

    if (exec_env) wasm_runtime_destroy_exec_env(exec_env);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
    if (wasm_buf) free(wasm_buf);
    wasm_runtime_destroy();

    ESP_LOGI(TAG, "free heap after destroy: %u (WAMR pool is static: %u bytes)",
             (unsigned)esp_get_free_heap_size(), (unsigned)sizeof(s_wamr_heap));
    ESP_LOGI(TAG, "selftest %s", ok ? "PASS" : "FAIL");
    return ok;
}

// ---- Phase 2: demo ----

namespace {

void* demo_thread(void*)
{
    const size_t heap_before = esp_get_free_heap_size();

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = s_wamr_heap;
    init_args.mem_alloc_option.pool.heap_size = sizeof(s_wamr_heap);

    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "demo: wasm_runtime_full_init failed");
        return nullptr;
    }
    if (!hostapi_register_natives()) {
        wasm_runtime_destroy();
        return nullptr;
    }

    char error_buf[128];
    const uint32_t wasm_size = (uint32_t)(demo_wasm_end - demo_wasm_start);
    // interpreter はバッファを module 生存中参照する。デモは常駐なので保持し続ける
    uint8_t* wasm_buf = (uint8_t*)malloc(wasm_size);
    if (!wasm_buf) {
        ESP_LOGE(TAG, "demo: malloc for wasm buf failed");
        wasm_runtime_destroy();
        return nullptr;
    }
    memcpy(wasm_buf, demo_wasm_start, wasm_size);

    wasm_module_t module =
        wasm_runtime_load(wasm_buf, wasm_size, error_buf, sizeof(error_buf));
    if (!module) {
        ESP_LOGE(TAG, "demo: load failed: %s", error_buf);
        return nullptr;
    }
    wasm_module_inst_t inst = wasm_runtime_instantiate(
        module, 8 * 1024, 8 * 1024, error_buf, sizeof(error_buf));
    if (!inst) {
        ESP_LOGE(TAG, "demo: instantiate failed: %s", error_buf);
        return nullptr;
    }
    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, 8 * 1024);
    if (!exec_env) {
        ESP_LOGE(TAG, "demo: create_exec_env failed");
        return nullptr;
    }

    wasm_function_inst_t fn_init = wasm_runtime_lookup_function(inst, "app_init");
    wasm_function_inst_t fn_tick = wasm_runtime_lookup_function(inst, "app_tick");
    if (!fn_init || !fn_tick) {
        ESP_LOGE(TAG, "demo: app_init/app_tick not exported");
        return nullptr;
    }

    uint32_t argv[1] = {0};
    if (!wasm_runtime_call_wasm(exec_env, fn_init, 0, argv)) {
        ESP_LOGE(TAG, "demo: app_init failed: %s", wasm_runtime_get_exception(inst));
        return nullptr;
    }
    ESP_LOGI(TAG, "demo: app_init() = %d, free heap %u (delta %d), tick loop start",
             (int)argv[0], (unsigned)esp_get_free_heap_size(),
             (int)(heap_before - esp_get_free_heap_size()));

    while (true) {
        if (!wasm_runtime_call_wasm(exec_env, fn_tick, 0, nullptr)) {
            ESP_LOGE(TAG, "demo: app_tick trapped: %s",
                     wasm_runtime_get_exception(inst));
            return nullptr;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

} // namespace

bool run_demo()
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 16 * 1024;
    cfg.thread_name = "wasm_demo";
    cfg.prio = 5;
    esp_pthread_set_cfg(&cfg);

    pthread_t th;
    if (pthread_create(&th, nullptr, demo_thread, nullptr) != 0) {
        ESP_LOGE(TAG, "failed to create wasm_demo pthread");
        return false;
    }
    pthread_detach(th);
    return true;
}

// WAMR の esp-idf プラットフォーム層は pthread_self() を使うため、
// 実行スレッドは pthread として起こす必要がある(素の xTaskCreate だと
// ESP-IDF の pthread_self が assert する)。
bool run_selftest_task()
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 16 * 1024;
    cfg.thread_name = "wasm_test";
    esp_pthread_set_cfg(&cfg);

    pthread_t th;
    auto thread_fn = [](void*) -> void* {
        return run_selftest() ? (void*)1 : nullptr;
    };
    if (pthread_create(&th, nullptr, thread_fn, nullptr) != 0) {
        ESP_LOGE(TAG, "failed to create wasm_test pthread");
        return false;
    }
    void* ret = nullptr;
    pthread_join(th, &ret);
    return ret == (void*)1;
}

} // namespace wasmrt
