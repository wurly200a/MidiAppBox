// WASM アプリランチャー: SD 準備(Phase 5A)。メニュー UI は Phase 5B で追加。
#include "launcher.hpp"

#include "sdcard.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/stat.h>

static const char* TAG = "WASM/LAUNCH";

// 初回セットアップ用の埋め込みサンプルアプリ
extern const uint8_t demo_wasm_start[] asm("_binary_demo_wasm_start");
extern const uint8_t demo_wasm_end[]   asm("_binary_demo_wasm_end");

namespace {

storage::SdCard s_sd;
bool s_mounted = false;

// path が存在しなければ埋め込みバイナリを書き込む
void seed_file(const char* path, const uint8_t* start, const uint8_t* end)
{
    struct stat st;
    if (stat(path, &st) == 0) return; // 既にある

    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "seed: cannot create %s", path);
        return;
    }
    const size_t size = (size_t)(end - start);
    const size_t written = fwrite(start, 1, size, f);
    fclose(f);
    ESP_LOGI(TAG, "seed: wrote %s (%u/%u bytes)", path, (unsigned)written,
             (unsigned)size);
}

} // namespace

namespace wasmrt {

bool launcher_prepare_sd(char* status, size_t status_len)
{
    if (!s_mounted) {
        ESP_LOGI(TAG, "heap: free=%u largest=%u | DMA: free=%u largest=%u | internal: free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        // 起動直後はカードの準備ができておらずタイムアウトすることがあるため
        // 少し待ってからリトライする
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(400));
            if (s_sd.mount("/sdcard")) {
                s_mounted = true;
                break;
            }
            ESP_LOGW(TAG, "SD mount attempt %d failed", attempt + 1);
        }
        if (!s_mounted) {
            snprintf(status, status_len, "SD mount failed");
            return false;
        }
    }

    struct stat st;
    if (stat(kAppsDir, &st) != 0) {
        if (mkdir(kAppsDir, 0775) != 0) {
            snprintf(status, status_len, "cannot create %s", kAppsDir);
            return false;
        }
        ESP_LOGI(TAG, "created %s", kAppsDir);
    }

    // 初回セットアップ: サンプルアプリを配置
    char path[64];
    snprintf(path, sizeof(path), "%s/demo.wasm", kAppsDir);
    seed_file(path, demo_wasm_start, demo_wasm_end);

    snprintf(status, status_len, "SD ready");
    return true;
}

} // namespace wasmrt
