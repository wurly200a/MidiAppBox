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
#include <cstdio>
#include <algorithm>
#include <atomic>

static const char* TAG = "WASM";

// WAMR グローバルヒーププール(内部 SRAM, BSS)。
// Phase 4 実測でデモ規模の消費は ~27KB。Phase 7B でクリックタスク等の静的確保が
// 増えた際、system heap の最大連続ブロックが 15KB まで細り linear memory
// (~20KB 連続)が確保できなくなったため、64KB → 48KB に縮小して静的メモリを
// 返却(実測消費に対し余裕 ~20KB)。プール枯渇時は instantiate が
// 「allocate memory failed」(linear ではなく)で落ちるので区別できる。
static uint8_t s_wamr_heap[48 * 1024];

namespace wasmrt {

// ---- Phase 15: メモリ報告のヘルパ(常設)----
//
// PSRAM を有効にすると esp_get_free_heap_size() と MALLOC_CAP_DEFAULT の
// largest free block は PSRAM を含んだ 8MB 級の値になり、「internal が枯れて
// いないか」「リークしていないか」のどちらも表さなくなる。回帰が見たいのは
// その 2 つなので、報告は internal / PSRAM の 2 系統に分けて出す。
// 詳細は docs/design/phase15-psram.md §4。

namespace {

constexpr uint32_t kCapsInt = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

const char* heap_mem_where(const void* p)
{
    const uintptr_t a = (uintptr_t)p;
    if (a >= 0x3C000000u && a < 0x3E000000u) return "PSRAM";
    if (a >= 0x3FC00000u && a < 0x3FD00000u) return "internal DRAM";
    return "?";
}

void log_heap(const char* what)
{
    ESP_LOGI(TAG, "%s: free_int=%u largest_int=%u free_psram=%u largest_psram=%u",
             what,
             (unsigned)heap_caps_get_free_size(kCapsInt),
             (unsigned)heap_caps_get_largest_free_block(kCapsInt),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

} // namespace



// ---- tick ジッタ計測(常設。Phase 4 §2 由来)----

namespace {

// 最初の kJitterSamples 回の起床間隔と app_tick 実行時間を集めて統計をログする。
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

// ---- Phase 5: ランタイム常駐+アプリライフサイクル ----

bool runtime_init()
{
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = s_wamr_heap;
    init_args.mem_alloc_option.pool.heap_size = sizeof(s_wamr_heap);

    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "wasm_runtime_full_init failed");
        return false;
    }
    if (!hostapi_register_natives()) {
        wasm_runtime_destroy();
        return false;
    }
    ESP_LOGI(TAG, "runtime ready (pool %u bytes), free heap %u",
             (unsigned)sizeof(s_wamr_heap), (unsigned)heap_caps_get_free_size(kCapsInt));
    return true;
}

namespace {

enum class AppState { Idle, Running, StopRequested };

std::atomic<AppState> s_app_state{AppState::Idle};
char s_app_path[160];
AppStoppedCb s_on_stopped = nullptr;
char s_app_error[160];

// SD 上のファイルを malloc したバッファへ読む。失敗時 nullptr。
uint8_t* read_wasm_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(s_app_error, sizeof(s_app_error), "cannot open %s", path);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 512 * 1024) {
        snprintf(s_app_error, sizeof(s_app_error), "bad file size (%ld)", size);
        fclose(f);
        return nullptr;
    }
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        snprintf(s_app_error, sizeof(s_app_error), "read failed: %s", path);
        free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    *out_size = (uint32_t)size;
    return buf;
}

void* app_thread(void*)
{
    const size_t free_int_at_start = heap_caps_get_free_size(kCapsInt);
    const size_t free_psram_at_start = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const char* error = nullptr;
    char error_buf[128];

    hostapi_audio_reset(); // アプリは必ず STOPPED 状態から始まる

    // interpreter はバッファを module 生存中参照する(fast-interp は in-place
    // 書き換えもする)ので、unload まで保持する
    uint32_t wasm_size = 0;
    uint8_t* wasm_buf = read_wasm_file(s_app_path, &wasm_size);

    wasm_module_t module = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t exec_env = nullptr;

    do {
        if (!wasm_buf) {
            error = s_app_error;
            break;
        }
        ESP_LOGI(TAG, "app: loading %s (%u bytes)", s_app_path, (unsigned)wasm_size);

        module = wasm_runtime_load(wasm_buf, wasm_size, error_buf, sizeof(error_buf));
        if (!module) {
            snprintf(s_app_error, sizeof(s_app_error), "load: %s", error_buf);
            error = s_app_error;
            break;
        }
        inst = wasm_runtime_instantiate(module, 8 * 1024, 8 * 1024,
                                        error_buf, sizeof(error_buf));
        if (!inst) {
            snprintf(s_app_error, sizeof(s_app_error), "instantiate: %s", error_buf);
            error = s_app_error;
            break;
        }
        exec_env = wasm_runtime_create_exec_env(inst, 8 * 1024);
        if (!exec_env) {
            error = "create_exec_env failed";
            break;
        }

        // Phase 15(常設): linear memory の確保先を毎回ログに残す。
        // 先頭バイトが 0x3c/0x3d なら PSRAM、0x3fc なら internal DRAM。
        // **largest free block は WASM の可否を表さない**(Phase 15 の教訓)ので、
        // 「PSRAM から取れているか」はこのアドレスで見張る。将来 WAMR / IDF の
        // 更新で os_mmap の確保 caps が変わったら、ここが 0x3fc… に戻って気づける。
        {
            wasm_memory_inst_t mem = wasm_runtime_get_memory(inst, 0);
            if (mem) {
                const uint64_t pages = wasm_memory_get_cur_page_count(mem);
                const uint64_t bpp = wasm_memory_get_bytes_per_page(mem);
                ESP_LOGI(TAG, "app: linear memory %p size %llu (%s), wasm buf %p size %u",
                         wasm_memory_get_base_address(mem),
                         (unsigned long long)(pages * bpp),
                         heap_mem_where(wasm_memory_get_base_address(mem)),
                         (void*)wasm_buf, (unsigned)wasm_size);
            }
            else {
                ESP_LOGW(TAG, "app: wasm_runtime_get_memory(0) returned NULL");
            }
            log_heap("app: started");
        }

        wasm_function_inst_t fn_init = wasm_runtime_lookup_function(inst, "app_init");
        wasm_function_inst_t fn_tick = wasm_runtime_lookup_function(inst, "app_tick");
        wasm_function_inst_t fn_exit = wasm_runtime_lookup_function(inst, "app_exit");
        if (!fn_init || !fn_tick) {
            error = "app_init/app_tick not exported";
            break;
        }

        uint32_t argv[1] = {0};
        if (!wasm_runtime_call_wasm(exec_env, fn_init, 0, argv)) {
            snprintf(s_app_error, sizeof(s_app_error), "app_init: %s",
                     wasm_runtime_get_exception(inst));
            error = s_app_error;
            break;
        }
        ESP_LOGI(TAG, "app: app_init() = %d, free heap %u, tick loop start",
                 (int)argv[0], (unsigned)heap_caps_get_free_size(kCapsInt));

#if CONFIG_WAMR_ENABLE_MEMORY_PROFILING
        wasm_runtime_dump_mem_consumption(exec_env);
#endif

        // 周期は vTaskDelayUntil による絶対時刻基準(音楽アプリ想定の周期駆動)
        TickType_t last_wake = xTaskGetTickCount();
        int64_t prev_start_us = 0;
        int sample_idx = 0;
        while (s_app_state.load() == AppState::Running) {
            const int64_t start_us = esp_timer_get_time();
            if (!wasm_runtime_call_wasm(exec_env, fn_tick, 0, nullptr)) {
                snprintf(s_app_error, sizeof(s_app_error), "app_tick: %s",
                         wasm_runtime_get_exception(inst));
                error = s_app_error;
                break;
            }
            const int64_t end_us = esp_timer_get_time();

            // Phase 4 由来の計測(常設): 最初の kJitterSamples 回の統計
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

        // 正常停止時のみ、export されていれば app_exit() を呼ぶ(結果は不問)
        if (!error && fn_exit) {
            if (!wasm_runtime_call_wasm(exec_env, fn_exit, 0, nullptr)) {
                ESP_LOGW(TAG, "app: app_exit trapped: %s",
                         wasm_runtime_get_exception(inst));
            }
        }
    } while (false);

    // ライフサイクル契約: アプリ破棄時は再生中のオーディオを必ず停止する
    hostapi_audio_reset();

    // 破棄は必ずこの順序: exec_env → instance → module → wasm バッファ
    if (exec_env) wasm_runtime_destroy_exec_env(exec_env);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
    if (wasm_buf) free(wasm_buf);

    // 1 行目は device-regress.sh が読む書式(free heap / largest block の語を維持)。
    // Phase 15 で値の意味を **internal 基準**へ改めた(PSRAM 込みの合計では
    // internal の逼迫もリークも見えないため)。2 行目が 4 値の正本。
    ESP_LOGI(TAG, "app: stopped (%s), free heap %u (at start %u), largest block %u",
             error ? error : "ok",
             (unsigned)heap_caps_get_free_size(kCapsInt),
             (unsigned)free_int_at_start,
             (unsigned)heap_caps_get_largest_free_block(kCapsInt));
    ESP_LOGI(TAG,
             "app: stopped free_int=%u largest_int=%u free_psram=%u largest_psram=%u"
             " [start free_int=%u free_psram=%u]",
             (unsigned)heap_caps_get_free_size(kCapsInt),
             (unsigned)heap_caps_get_largest_free_block(kCapsInt),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)free_int_at_start, (unsigned)free_psram_at_start);

    // コールバック完了後に Idle へ遷移する(Idle を見て次のアプリを起動する側と、
    // コールバック内の画面後始末が競合しないように)
    if (s_on_stopped) s_on_stopped(error);
    s_app_state.store(AppState::Idle);
    return nullptr;
}

} // namespace

bool app_start(const char* path, AppStoppedCb on_stopped)
{
    AppState expected = AppState::Idle;
    if (!s_app_state.compare_exchange_strong(expected, AppState::Running)) {
        ESP_LOGW(TAG, "app_start: another app is still active");
        return false;
    }
    strlcpy(s_app_path, path, sizeof(s_app_path));
    s_on_stopped = on_stopped;
    s_app_error[0] = '\0';

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 16 * 1024;
    cfg.thread_name = "wasm_app";
    cfg.prio = 5;
    esp_pthread_set_cfg(&cfg);

    pthread_t th;
    if (pthread_create(&th, nullptr, app_thread, nullptr) != 0) {
        ESP_LOGE(TAG, "failed to create wasm_app pthread");
        s_app_state.store(AppState::Idle);
        return false;
    }
    pthread_detach(th);
    return true;
}

void app_request_stop()
{
    AppState expected = AppState::Running;
    s_app_state.compare_exchange_strong(expected, AppState::StopRequested);
}

bool app_is_running()
{
    return s_app_state.load() != AppState::Idle;
}

} // namespace wasmrt
