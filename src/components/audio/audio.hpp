#pragma once
#include <string>
#include <atomic>
#include <cstdint>
#include <cstdio>
#if __has_include("audio_player.h")
#define HAVE_ESP_AUDIO_PLAYER 1
extern "C" {
#include "audio_player.h"
}
#else
#define HAVE_ESP_AUDIO_PLAYER 0
#endif
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// MP3 playback using espressif/audio_player + I2S (std mode)

namespace audio {

class Mp3Player {
public:
    struct Pins {
        // I2S pin mapping (defaults based on the provided PCM5101 example)
        gpio_num_t bclk = GPIO_NUM_48;  // SCLK/BCLK
        gpio_num_t ws   = GPIO_NUM_38;  // L/RCLK (LRCK/WS)
        gpio_num_t dout = GPIO_NUM_47;  // SDOUT to DAC
        gpio_num_t mclk = GPIO_NUM_NC;  // Not used by PCM5101 in many cases
        gpio_num_t din  = GPIO_NUM_NC;  // Not used (no RX)
    };

    Mp3Player() noexcept;                  // uses default pins
    explicit Mp3Player(const Pins& pins) noexcept;
    ~Mp3Player();

    bool init(uint32_t sample_rate_hz = 44100, uint8_t bits = 16, bool stereo = true) noexcept;

    // Start playback of a file via audio_player when available (fallback stubs otherwise)
    bool play_file(const std::string& path) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void stop() noexcept;

    void set_volume(uint8_t vol_0_100) noexcept; // 0..100
    uint8_t volume() const noexcept { return volume_.load(); }

    bool is_playing() const noexcept;
    bool is_paused() const noexcept;
    bool finished_flag() const noexcept { return finished_.load(); }

private:
    bool ensure_i2s(uint32_t rate_hz, uint8_t bits, bool stereo) noexcept;
    bool reconfig_rate(uint32_t rate_hz, uint32_t bits_cfg, i2s_slot_mode_t ch) noexcept;
    bool i2s_write(void* data, size_t len, uint32_t timeout_ms, size_t* written = nullptr) noexcept;
#if HAVE_ESP_AUDIO_PLAYER
    static esp_err_t write_fn(void* audio_buffer, size_t len, size_t* bytes_written, uint32_t timeout_ms);
    static esp_err_t clk_set_fn(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);
    static esp_err_t mute_fn(AUDIO_PLAYER_MUTE_SETTING setting);
    static void player_callback(audio_player_cb_ctx_t* ctx);
#endif

private:
    Pins pins_{};
    i2s_chan_handle_t tx_ = nullptr;
    i2s_chan_handle_t rx_ = nullptr; // unused
    std::string current_path_;
    std::atomic<bool> finished_{false};
    std::atomic<uint8_t> volume_{98}; // default near max

    // Current i2s format
    uint32_t cur_rate_ = 44100;
    uint8_t  cur_bits_ = 16;
    bool     cur_stereo_ = true;

    // audio_player synchronization
    static Mp3Player* s_self; // for static callbacks
    QueueHandle_t event_queue_ = nullptr;
#if HAVE_ESP_AUDIO_PLAYER
    audio_player_callback_event_t expected_event_{};
    audio_player_callback_event_t event_{};
#endif
    FILE* file_ = nullptr;
};

// Optional C-style wrappers mirroring the provided C API names
extern "C" {
    void Audio_Init(void);
    void Play_Music(const char* directory, const char* fileName);
    void Music_resume(void);
    void Music_pause(void);
    void Music_stop(void);
    bool Music_is_playing(void);
    void Volume_adjustment(uint8_t Vol);
    extern uint8_t Audio_Volume;      // 0..100
    extern bool    Music_Next_Flag;   // Set true when file finished
}

} // namespace audio
