#include "wasm_runtime.hpp"
#include "hostapi.hpp"

#include "wasm_export.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_pthread.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <pthread.h>
#include <cstring>
#include <algorithm>

static const char* TAG = "WASM";

// EMBED_FILES で埋め込んだ .wasm
extern const uint8_t hello_wasm_start[] asm("_binary_hello_wasm_start");
extern const uint8_t hello_wasm_end[]   asm("_binary_hello_wasm_end");
extern const uint8_t demo_wasm_start[] asm("_binary_demo_wasm_start");
extern const uint8_t demo_wasm_end[]   asm("_binary_demo_wasm_end");
extern const uint8_t bench_wasm_start[] asm("_binary_bench_wasm_start");
extern const uint8_t bench_wasm_end[]   asm("_binary_bench_wasm_end");

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

// ---- Phase 4: 計測 ----

namespace {

// ホスト API 呼び出しコスト計測(Phase 4 §1)。
// ランタイム初期化・natives 登録済みの状態で demo_thread から呼ぶ。
void run_bench_module()
{
    char error_buf[128];
    const uint32_t wasm_size = (uint32_t)(bench_wasm_end - bench_wasm_start);
    uint8_t* wasm_buf = (uint8_t*)malloc(wasm_size);
    if (!wasm_buf) return;
    memcpy(wasm_buf, bench_wasm_start, wasm_size);

    wasm_module_t module =
        wasm_runtime_load(wasm_buf, wasm_size, error_buf, sizeof(error_buf));
    if (!module) {
        ESP_LOGE(TAG, "bench: load failed: %s", error_buf);
        free(wasm_buf);
        return;
    }
    wasm_module_inst_t inst = wasm_runtime_instantiate(
        module, 8 * 1024, 8 * 1024, error_buf, sizeof(error_buf));
    wasm_exec_env_t exec_env =
        inst ? wasm_runtime_create_exec_env(inst, 8 * 1024) : nullptr;
    wasm_function_inst_t fn_empty =
        inst ? wasm_runtime_lookup_function(inst, "bench_empty") : nullptr;
    wasm_function_inst_t fn_host =
        inst ? wasm_runtime_lookup_function(inst, "bench_hostcall") : nullptr;

    if (exec_env && fn_empty && fn_host) {
        constexpr uint32_t kLoopN = 100000;
        constexpr uint32_t kInvokeN = 1000;
        uint32_t argv[1];

        // (1) host→wasm の関数呼び出しオーバーヘッド: bench_empty(0) を N 回
        uint32_t c0 = esp_cpu_get_cycle_count();
        for (uint32_t i = 0; i < kInvokeN; i++) {
            argv[0] = 0;
            wasm_runtime_call_wasm(exec_env, fn_empty, 1, argv);
        }
        uint32_t c_invoke = esp_cpu_get_cycle_count() - c0;

        // (2) 純 wasm ループ: bench_empty(N)
        argv[0] = kLoopN;
        c0 = esp_cpu_get_cycle_count();
        wasm_runtime_call_wasm(exec_env, fn_empty, 1, argv);
        uint32_t c_empty = esp_cpu_get_cycle_count() - c0;
        uint32_t r_empty = argv[0];

        // (3) wasm→host 呼び出し込みループ: bench_hostcall(N)
        argv[0] = kLoopN;
        c0 = esp_cpu_get_cycle_count();
        wasm_runtime_call_wasm(exec_env, fn_host, 1, argv);
        uint32_t c_host = esp_cpu_get_cycle_count() - c0;
        uint32_t r_host = argv[0];

        // (4) ネイティブ基準: 同じ処理 (esp_timer 由来の ms 取得) を C で N 回
        volatile uint32_t sink = 0;
        c0 = esp_cpu_get_cycle_count();
        for (uint32_t i = 0; i < kLoopN; i++) {
            sink += (uint32_t)(esp_timer_get_time() / 1000);
        }
        uint32_t c_native = esp_cpu_get_cycle_count() - c0;

        const uint32_t cpu_mhz = 160; // sdkconfig: CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
        ESP_LOGI(TAG, "bench: host->wasm invoke: %u cycles/call (%.2f us)",
                 c_invoke / kInvokeN, (float)(c_invoke / kInvokeN) / cpu_mhz);
        ESP_LOGI(TAG, "bench: wasm loop body: %.1f cycles/iter",
                 (float)c_empty / kLoopN);
        ESP_LOGI(TAG, "bench: wasm->host call (now_ms): %.1f cycles/call (%.2f us)",
                 (float)(c_host - c_empty) / kLoopN,
                 (float)(c_host - c_empty) / kLoopN / cpu_mhz);
        ESP_LOGI(TAG, "bench: native now_ms baseline: %.1f cycles/call (%.2f us)",
                 (float)c_native / kLoopN, (float)c_native / kLoopN / cpu_mhz);
        ESP_LOGI(TAG, "bench: (checksums empty=%u host=%u sink=%u)",
                 r_empty, r_host, (unsigned)sink);
    } else {
        ESP_LOGE(TAG, "bench: setup failed (%s)",
                 inst ? "exports missing" : error_buf);
    }

    if (exec_env) wasm_runtime_destroy_exec_env(exec_env);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
    free(wasm_buf);
}

// tick ジッタ計測(Phase 4 §2)。最初の kJitterSamples 回の
// 起床間隔と app_tick 実行時間を集めて統計をログする。
constexpr int kJitterSamples = 1000;
uint32_t s_intervals_us[kJitterSamples];
uint32_t s_durations_us[kJitterSamples];

void log_stats(const char* name, uint32_t* v, int n)
{
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    std::sort(v, v + n);
    ESP_LOGI(TAG,
             "jitter: %s min=%u avg=%u p50=%u p95=%u p99=%u max=%u us (n=%d)",
             name, v[0], (uint32_t)(sum / n), v[n / 2], v[(int)(n * 0.95)],
             v[(int)(n * 0.99)], v[n - 1], n);
}

} // namespace

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

    // Phase 4: ホスト API 呼び出しコスト計測(デモ開始前に一度)
    run_bench_module();

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

#if CONFIG_WAMR_ENABLE_MEMORY_PROFILING
    // Phase 4: WAMR 内部のメモリ消費(プロファイリング有効ビルドのみ関数が存在する)
    wasm_runtime_dump_mem_consumption(exec_env);
#endif

    // 周期は vTaskDelayUntil による絶対時刻基準(音楽アプリ想定の周期駆動)
    TickType_t last_wake = xTaskGetTickCount();
    int64_t prev_start_us = 0;
    int sample_idx = 0;
    while (true) {
        const int64_t start_us = esp_timer_get_time();
        if (!wasm_runtime_call_wasm(exec_env, fn_tick, 0, nullptr)) {
            ESP_LOGE(TAG, "demo: app_tick trapped: %s",
                     wasm_runtime_get_exception(inst));
            return nullptr;
        }
        const int64_t end_us = esp_timer_get_time();

        // Phase 4: 最初の kJitterSamples 回の起床間隔と実行時間を収集
        if (sample_idx < kJitterSamples) {
            if (prev_start_us != 0) {
                s_intervals_us[sample_idx] = (uint32_t)(start_us - prev_start_us);
                s_durations_us[sample_idx] = (uint32_t)(end_us - start_us);
                sample_idx++;
                if (sample_idx == kJitterSamples) {
                    log_stats("tick interval (target 100000)", s_intervals_us,
                              kJitterSamples);
                    log_stats("app_tick duration", s_durations_us, kJitterSamples);
                }
            }
            prev_start_us = start_us;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
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
