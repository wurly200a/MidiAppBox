#pragma once
#include <stdint.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 1) Move Config outside the class (name unchanged is fine)
struct PowerKeyConfig {
    gpio_num_t key_pin            = GPIO_NUM_6;  // Power button input
    gpio_num_t latch_pin          = GPIO_NUM_7;  // Self-hold control
    bool       key_active_low     = true;        // Active-low (pressed = 0)
    bool       use_internal_pullup= true;        // Recommend true when active-low
    uint32_t   debounce_ms        = 50;          // Debounce
    uint32_t   hold_ms            = 2000;        // Long-press shutdown threshold (ms)
    uint32_t   poll_period_ms     = 10;          // Poll period when using a task
    bool       use_deepsleep_hold = true;        // Keep level during deep sleep after OFF
};

class PowerKey final {
public:
    using Config = PowerKeyConfig; // Add compatibility alias

  // 2) Keep default argument as-is
    explicit PowerKey(PowerKeyConfig cfg = PowerKeyConfig{}) noexcept;

    void init() noexcept;
    void poll() noexcept;
    void start_task(UBaseType_t prio = 5, uint32_t stack = 2048) noexcept;

    bool is_battery_mode()  const noexcept { return battery_mode_; }
    bool shutdown_issued()  const noexcept { return shutdown_issued_; }

    const PowerKeyConfig& config() const noexcept { return cfg_; }
    void set_config(const PowerKeyConfig& cfg) noexcept { cfg_ = cfg; }

private:
    static void task_trampoline(void* arg) noexcept;
    bool read_raw() const noexcept;
    bool debounce_read() noexcept;
    void cut_latch_and_maybe_sleep() noexcept;
    static inline void gpio_conf(gpio_num_t pin, gpio_mode_t mode) noexcept;

private:
    PowerKeyConfig cfg_{};       // Store the externalized type as a member
    bool       battery_mode_      = false;
    bool       pressed_stable_    = false;
    bool       raw_prev_          = false;
    TickType_t raw_edge_ts_       = 0;
    TickType_t press_start_       = 0;
    volatile bool shutdown_issued_ = false;
    TaskHandle_t task_            = nullptr;
};
