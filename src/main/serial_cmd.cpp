// USB Serial/JTAG 上のコマンドコンソール(Phase 12 作業 3)。
//
// 目的は実機回帰の自動化で、ランチャーのタッチ・電源キーという人手前提の経路を
// 置き換えるものではない(既存の操作系はそのまま残る)。
//
// 経路について: このボードの /dev/ttyACM0 は ESP32-S3 内蔵の USB Serial/JTAG で、
// ESP-IDF のコンソール設定では primary=UART0 / secondary=USB Serial/JTAG になって
// いる。secondary console は出力専用なので stdin には何も届かない。そこで
// コンソール設定には触れず、USJ ドライバを直接入れて read する。実測(Phase 12)で
//   - ドライバ導入後もログ出力は影響を受けない
//   - `idf.py monitor` 経由でホストから送った文字がそのまま届く(行末は CR)
//   - 38 文字 x 5 行を待ちなしで送ってもバイト欠落なし
// を確認済み。
//
// 応答は printf ではなく ESP_LOG で出す。printf(stdout)は primary console
// = UART0 に出てしまい、USB 側には現れないため。
//
// タスク優先度は 2(audio_player の 3 より低い。MP3 再生と共存する常駐タスクの
// 教訓 P10-1)。スタックは静的確保(恒久物をヒープから取ると最大連続ブロックを
// 分断する。教訓 6B/7B-fix)。ロックは LVGL のもの(launcher_show と同じ流儀)
// だけで、L0 ディスパッチャの portMUX とは共有しない。

#include "serial_cmd.hpp"

#include "sdkconfig.h"

#if CONFIG_MIDIBOX_SERIAL_CMD

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "launcher.hpp"
#include "wasm_runtime.hpp"

#include <cstring>
#include <cstdio>
#include <dirent.h>

namespace {

// 応答行のタグ。回帰スクリプトはこのタグで行を拾う。
const char* TAG = "MBCMD";

constexpr int kStackWords = 3072;
constexpr size_t kLineMax = 96;
constexpr size_t kRxChunk = 64;

StaticTask_t s_tcb;
StackType_t s_stack[kStackWords];
char s_line[kLineMax];
size_t s_line_len = 0;

void cmd_heap()
{
    ESP_LOGI(TAG, "heap free %u largest %u min %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void cmd_ls()
{
    DIR* dir = opendir(wasmrt::kAppsDir);
    if (!dir) {
        ESP_LOGI(TAG, "ls err apps dir not found");
        return;
    }
    int count = 0;
    while (dirent* ent = readdir(dir)) {
        const size_t len = strlen(ent->d_name);
        if (len < 6 || strcasecmp(ent->d_name + len - 5, ".wasm") != 0) continue;
        ESP_LOGI(TAG, "app %s", ent->d_name);
        count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "ls done %d", count);
}

void cmd_run(const char* name)
{
    const char* err = "unknown";
    char path[96];
    if (wasmrt::launcher_launch_by_name(name, &err, path, sizeof(path))) {
        ESP_LOGI(TAG, "run ok %s", path);
    } else {
        ESP_LOGI(TAG, "run err %s", err);
    }
}

void cmd_stop()
{
    if (!wasmrt::app_is_running()) {
        ESP_LOGI(TAG, "stop idle");
        return;
    }
    wasmrt::app_request_stop();
    ESP_LOGI(TAG, "stop ok");
}

void dispatch(char* line)
{
    // 前後の空白を落とす
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    if (n == 0) return;

    // 最初の語がコマンド、残りが引数
    char* arg = strchr(line, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ') arg++;
    }

    if (strcmp(line, "ping") == 0)      ESP_LOGI(TAG, "pong");
    else if (strcmp(line, "heap") == 0) cmd_heap();
    else if (strcmp(line, "ls") == 0)   cmd_ls();
    else if (strcmp(line, "stop") == 0) cmd_stop();
    else if (strcmp(line, "run") == 0)  cmd_run(arg ? arg : "");
    else ESP_LOGI(TAG, "err unknown command '%s'", line);
}

void console_task(void*)
{
    static uint8_t rx[kRxChunk];
    for (;;) {
        const int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) {
            const char c = (char)rx[i];
            if (c == '\r' || c == '\n') {
                s_line[s_line_len] = 0;
                dispatch(s_line);
                s_line_len = 0;
            } else if (s_line_len + 1 < sizeof(s_line)) {
                s_line[s_line_len++] = c;
            } else {
                // 行が長すぎる。捨てて次の行末まで読み飛ばす
                s_line_len = 0;
                ESP_LOGI(TAG, "err line too long");
            }
        }
    }
}

} // namespace

namespace serialcmd {

void Init()
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        return;
    }
    xTaskCreateStatic(console_task, "serial_cmd", kStackWords, nullptr, 2, s_stack, &s_tcb);
    ESP_LOGI(TAG, "ready");
}

} // namespace serialcmd

#else // !CONFIG_MIDIBOX_SERIAL_CMD

namespace serialcmd {
void Init() {}
} // namespace serialcmd

#endif
