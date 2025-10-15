#include "power_key.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include <inttypes.h>

static const char* TAG = "PWR_KEY";

PowerKey::PowerKey(PowerKeyConfig cfg) noexcept : cfg_(cfg) {}

inline void PowerKey::gpio_conf(gpio_num_t pin, gpio_mode_t mode) noexcept {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, mode);
}

bool PowerKey::read_raw() const noexcept {
    int lvl = gpio_get_level(cfg_.key_pin);
    return cfg_.key_active_low ? (lvl == 0) : (lvl != 0);
}

bool PowerKey::debounce_read() noexcept {
    const bool raw = read_raw();
    const TickType_t now = xTaskGetTickCount();

    if (raw != raw_prev_) {
        raw_prev_    = raw;
        raw_edge_ts_ = now;
    }
    if (pdTICKS_TO_MS(now - raw_edge_ts_) >= cfg_.debounce_ms) {
        pressed_stable_ = raw;
    }
    return pressed_stable_;
}

void PowerKey::cut_latch_and_maybe_sleep() noexcept {
    if (shutdown_issued_) return;
    shutdown_issued_ = true;

    // 1) Drive self-hold LOW (cut power path)
    gpio_set_level(cfg_.latch_pin, 0);

    // 2) Optional: hold level during deep sleep to prevent accidental wake
    if (cfg_.use_deepsleep_hold) {
        gpio_hold_en(cfg_.latch_pin);
        gpio_deep_sleep_hold_en();
        ESP_LOGW(TAG, "Shutdown (>= %" PRIu32 " ms) -> latch LOW & deep sleep", cfg_.hold_ms);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        esp_deep_sleep_start(); // not expected to return
    } else {
        ESP_LOGW(TAG, "Shutdown (>= %" PRIu32 " ms) -> latch LOW", cfg_.hold_ms);
    }
}

void PowerKey::init() noexcept {
    // Input pin
    gpio_conf(cfg_.key_pin, GPIO_MODE_INPUT);
    if (cfg_.use_internal_pullup && cfg_.key_active_low) {
        gpio_set_pull_mode(cfg_.key_pin, GPIO_PULLUP_ONLY);
    } else {
        gpio_set_pull_mode(cfg_.key_pin, GPIO_FLOATING);
    }

    // Self-hold pin
    gpio_conf(cfg_.latch_pin, GPIO_MODE_OUTPUT);

    // Start with latch=LOW to stabilize
    gpio_set_level(cfg_.latch_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // If booted by key press (battery mode), set latch=HIGH to enable self-hold
    if (read_raw()) {
        battery_mode_ = true;
        gpio_set_level(cfg_.latch_pin, 1);
        ESP_LOGI(TAG, "Battery mode: latch ON (key pressed at boot)");
    } else {
        battery_mode_ = false; // external power
        ESP_LOGI(TAG, "External power: key control disabled");
    }

    // Initialize debounce
    raw_prev_       = read_raw();
    raw_edge_ts_    = xTaskGetTickCount();
    pressed_stable_ = raw_prev_;
    press_start_    = 0;
    shutdown_issued_= false;
}

void PowerKey::poll() noexcept {
    if (!battery_mode_ || shutdown_issued_) return;

    const bool pressed = debounce_read();
    const TickType_t now = xTaskGetTickCount();

    if (pressed) {
        if (press_start_ == 0) {
            press_start_ = now; // start of stable press
        }
        const uint32_t held_ms = pdTICKS_TO_MS(now - press_start_);
        if (held_ms >= cfg_.hold_ms) {
            // Power off immediately when threshold is reached, even if still pressed
            cut_latch_and_maybe_sleep();
        }
    } else {
        // Not pressed: reset continuous-press timer
        press_start_ = 0;
    }
}

void PowerKey::task_trampoline(void* arg) noexcept {
    auto* self = static_cast<PowerKey*>(arg);
    const TickType_t pd = pdMS_TO_TICKS(self->cfg_.poll_period_ms);
    for (;;) {
        self->poll();
        vTaskDelay(pd);
    }
}

void PowerKey::start_task(UBaseType_t prio, uint32_t stack) noexcept {
    if (task_) return;
    xTaskCreate(&PowerKey::task_trampoline, "pwr_key_loop", stack, this, prio, &task_);
}
