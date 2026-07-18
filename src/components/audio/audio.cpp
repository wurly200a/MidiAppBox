// Includes (kept minimal since header pulls most deps)
#include "audio.hpp"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include <cstring>
#include <cstdio>
#include <cmath>

namespace audio {

static const char* TAG = "AUDIO/MP3";

// Global player instance for C wrappers
static Mp3Player* g_player = nullptr;
Mp3Player* Mp3Player::s_self = nullptr;

extern "C" {
uint8_t Audio_Volume = 98;
bool    Music_Next_Flag = false;
}

Mp3Player::Mp3Player() noexcept : pins_(Pins{}) {}
Mp3Player::Mp3Player(const Pins& pins) noexcept : pins_(pins) {}

Mp3Player::~Mp3Player() {
    stop();
    if (tx_) { i2s_channel_disable(tx_); i2s_del_channel(tx_); tx_ = nullptr; }
    if (rx_) { i2s_channel_disable(rx_); i2s_del_channel(rx_); rx_ = nullptr; }
}

bool Mp3Player::init(uint32_t sample_rate_hz, uint8_t bits, bool stereo) noexcept {
    if (!ensure_i2s(sample_rate_hz, bits, stereo)) return false;
    ensure_click_task();
    s_self = this;
#if HAVE_ESP_AUDIO_PLAYER
    audio_player_config_t config{};
    config.mute_fn = &Mp3Player::mute_fn;
    config.write_fn = &Mp3Player::write_fn;
    config.clk_set_fn = &Mp3Player::clk_set_fn;
    config.priority = 3;
    config.coreID = tskNO_AFFINITY;
    if (audio_player_new(config) != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_new failed");
        return false;
    }
    if (audio_player_callback_register(&Mp3Player::player_callback, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "callback_register failed");
        return false;
    }
    event_queue_ = xQueueCreate(2, sizeof(audio_player_callback_event_t));
    if (!event_queue_) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return false;
    }
#else
    (void)event_queue_;
    ESP_LOGW(TAG, "esp_audio_player not available; using stubs");
#endif
    return true;
}

bool Mp3Player::ensure_i2s(uint32_t rate_hz, uint8_t bits, bool stereo) noexcept {
    if (tx_) {
        i2s_data_bit_width_t bw = (bits == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
        i2s_slot_mode_t sm = stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
        return reconfig_rate(rate_hz, (uint32_t)bw, sm);
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx_, &rx_) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }
    i2s_std_config_t std_cfg{};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz);
    std_cfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
        bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
        stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = pins_.mclk;
    std_cfg.gpio_cfg.bclk = pins_.bclk;
    std_cfg.gpio_cfg.ws   = pins_.ws;
    std_cfg.gpio_cfg.dout = pins_.dout;
    std_cfg.gpio_cfg.din  = pins_.din;
    std_cfg.gpio_cfg.invert_flags = { .mclk_inv=false, .bclk_inv=false, .ws_inv=false };
    if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        return false;
    }
    if (i2s_channel_enable(tx_) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed");
        return false;
    }
    cur_rate_ = rate_hz; cur_bits_ = bits; cur_stereo_ = stereo;
    return true;
}

bool Mp3Player::init_i2s_only(uint32_t sample_rate_hz, uint8_t bits, bool stereo) noexcept {
    if (!ensure_i2s(sample_rate_hz, bits, stereo)) return false;
    ensure_click_task();
    return true;
}

// 発音依頼(ノンブロッキング)。実書き込みは click_task_loop が行う。
// 二重クリック対策(Phase 7B fix): i2s_channel_write 直書きだと、クリック終端の
// DMA アンダーフロー時に auto_clear とプリフェッチが競合し、クリック先頭が入った
// 古いディスクリプタが 1 本再生される(実測: 全拍の ~25% で 26-27ms 後に再発音)。
// 対策はゼロパディング書き込み(下記)で、それがブロッキングになるため専用タスク化。
bool Mp3Player::play_click() noexcept {
    return play_tone(1000, 30, 100); // v0 既定クリック
}

bool Mp3Player::play_tone(uint16_t freq_hz, uint16_t dur_ms, uint8_t level) noexcept {
    if (!tone_queue_) return false;
    ToneMsg msg{freq_hz, dur_ms, level};
    // 満杯(発音が密に重なった)ときは捨てる(v2 契約: 重なりはベストエフォート)
    return xQueueSend(tone_queue_, &msg, 0) == pdTRUE;
}

// タスクスタック等は静的確保(BSS)。ヒープから取ると最大連続ブロックを
// 分断して WASM の linear memory 確保(~20KB 連続)を壊すため(6B の教訓)。
static uint8_t s_click_stack[4096];
static StaticTask_t s_click_tcb;
static uint8_t s_tone_queue_buf[4 * sizeof(Mp3Player::ToneMsg)];
static StaticQueue_t s_tone_queue_cb;

void Mp3Player::ensure_click_task() noexcept {
    if (click_task_) return;
    tone_queue_ = xQueueCreateStatic(4, sizeof(ToneMsg), s_tone_queue_buf,
                                     &s_tone_queue_cb);
    if (!tone_queue_) return;
    auto fn = [](void* arg) { static_cast<Mp3Player*>(arg)->click_task_loop(); };
    click_task_ = xTaskCreateStatic(fn, "click", sizeof(s_click_stack), this, 18,
                                    s_click_stack, &s_click_tcb);
    if (!click_task_) {
        ESP_LOGE(TAG, "click task create failed");
    }
}

void Mp3Player::click_task_loop() noexcept {
    ToneMsg msg;
    for (;;) {
        if (xQueueReceive(tone_queue_, &msg, portMAX_DELAY) == pdTRUE) {
            tone_write_now(msg);
        }
    }
}

// パラメトリック減衰サインをキャッシュレスで合成しながら書く (Phase 7C)。
// 再帰振動子(回転行列)なのでサンプルごとの libm 呼び出しは無い。
void Mp3Player::tone_write_now(const ToneMsg& msg) noexcept {
    if (!tx_) return;

    // MP3(22.05kHz 等)再生後に I2S レートが変わったままだと半分のピッチで
    // 鳴るため、発音前に 44.1kHz へ戻す(同一設定なら reconfig は no-op)
    if (cur_rate_ != 44100 || cur_bits_ != 16 || !cur_stereo_) {
        if (!ensure_i2s(44100, 16, true)) return;
    }

    constexpr int kSampleRate = 44100;
    const int total = (int)kSampleRate * msg.dur_ms / 1000;
    const float w = 2.0f * (float)M_PI * (float)msg.freq_hz / kSampleRate;
    const float cw = cosf(w);
    const float sw = sinf(w);
    // エンベロープ: dur_ms 終端で e^-3.5 ≒ -30dB(既定クリック 30ms は従来と同等)
    const float decay = expf(-3.5f / (float)total);
    float amp = 12000.0f * msg.level / 100.0f * volume_.load() / 100.0f;
    float s = 0.0f, c = 1.0f; // sin/cos の回転状態

    int16_t chunk[240 * 2]; // 1 DMA ディスクリプタぶん(タスクスタック上)
    int done = 0;
    while (done < total) {
        int n = total - done;
        if (n > 240) n = 240;
        for (int i = 0; i < n; ++i) {
            const float s2 = s * cw + c * sw;
            c = c * cw - s * sw;
            s = s2;
            amp *= decay;
            const int16_t v = (int16_t)(amp * s);
            chunk[i * 2] = v;
            chunk[i * 2 + 1] = v;
        }
        i2s_write(chunk, (size_t)n * 2 * sizeof(int16_t), 100);
        done += n;
    }

    // DMA リング(既定 6 ディスクリプタ × 240 フレーム)を丸ごとゼロで上書きする。
    // アンダーフロー時に古いディスクリプタがプリフェッチ再生されても無音になる。
    // 書き込みでブロックするが、専用タスクなので無害。
    static const int16_t zeros[240 * 2] = {};
    for (int i = 0; i < 6; ++i) {
        i2s_write((void*)zeros, sizeof(zeros), 100);
    }
}

bool Mp3Player::reconfig_rate(uint32_t rate_hz, uint32_t bits_cfg, i2s_slot_mode_t ch) noexcept {
    if (!tx_) return false;
    bool stereo = (ch == I2S_SLOT_MODE_STEREO);
    uint8_t bits = 16;
    if (bits_cfg == I2S_DATA_BIT_WIDTH_32BIT) bits = 32;
    else if (bits_cfg == I2S_DATA_BIT_WIDTH_24BIT) bits = 32; // use 32-slot for 24bit
    else bits = 16;
    if (cur_rate_ == rate_hz && cur_bits_ == bits && cur_stereo_ == stereo) return true;
    i2s_std_config_t std_cfg{};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz);
    std_cfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
        bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
        stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = pins_.mclk;
    std_cfg.gpio_cfg.bclk = pins_.bclk;
    std_cfg.gpio_cfg.ws   = pins_.ws;
    std_cfg.gpio_cfg.dout = pins_.dout;
    std_cfg.gpio_cfg.din  = pins_.din;
    std_cfg.gpio_cfg.invert_flags = { .mclk_inv=false, .bclk_inv=false, .ws_inv=false };
    if (i2s_channel_disable(tx_) != ESP_OK) return false;
    if (i2s_channel_reconfig_std_clock(tx_, &std_cfg.clk_cfg) != ESP_OK) return false;
    if (i2s_channel_reconfig_std_slot(tx_, &std_cfg.slot_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(tx_) != ESP_OK) return false;
    cur_rate_ = rate_hz; cur_bits_ = bits; cur_stereo_ = stereo;
    return true;
}

bool Mp3Player::i2s_write(void* data, size_t len, uint32_t timeout_ms, size_t* written) noexcept {
    size_t bw = 0;
    esp_err_t err = i2s_channel_write(tx_, data, len, &bw, timeout_ms);
    if (written) *written = bw;
    return err == ESP_OK;
}

#if HAVE_ESP_AUDIO_PLAYER
esp_err_t Mp3Player::write_fn(void* audio_buffer, size_t len, size_t* bytes_written, uint32_t timeout_ms)
{
    if (!s_self) return ESP_FAIL;
    int16_t* samples = static_cast<int16_t*>(audio_buffer);
    size_t sample_count = len / sizeof(int16_t);
    float volume_factor = (float)s_self->volume_.load() / 100.0f;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t v = (int32_t)std::lround((float)samples[i] * volume_factor);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        samples[i] = (int16_t)v;
    }
    return s_self->i2s_write(audio_buffer, len, timeout_ms, bytes_written) ? ESP_OK : ESP_FAIL;
}

esp_err_t Mp3Player::clk_set_fn(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    if (!s_self) return ESP_FAIL;
    return s_self->reconfig_rate(rate, bits_cfg, ch) ? ESP_OK : ESP_FAIL;
}
#endif

#if HAVE_ESP_AUDIO_PLAYER
esp_err_t Mp3Player::mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    ESP_LOGI(TAG, "mute setting %d", (int)setting);
    return ESP_OK;
}

void Mp3Player::player_callback(audio_player_cb_ctx_t* ctx)
{
    if (!s_self) return;
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        ESP_LOGI(TAG, "Playback finished");
        Music_Next_Flag = true;
        s_self->finished_.store(true);
        // FILE* is closed by audio_player; do not fclose() here
        s_self->file_ = nullptr;
    }
    if (ctx->audio_event == s_self->expected_event_) {
        xQueueSend(s_self->event_queue_, &(ctx->audio_event), 0);
    }
}
#endif

bool Mp3Player::play_file(const std::string& path) noexcept {
    // Pause current playback
    pause();
    // Do not fclose() here; audio_player thread owns previous FILE*
    if (file_) { file_ = nullptr; }
    file_ = fopen(path.c_str(), "rb");
    if (!file_) {
        ESP_LOGE(TAG, "Failed to open MP3 file: %s", path.c_str());
        return false;
    }
    // Do not perform ID3/scanning here; lower layer handles it
    current_path_ = path;
    finished_.store(false);
    Music_Next_Flag = false;
    
#if HAVE_ESP_AUDIO_PLAYER
    expected_event_ = AUDIO_PLAYER_CALLBACK_EVENT_PLAYING;
    esp_err_t ret = audio_player_play(file_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_play failed: %d", (int)ret);
        fclose(file_); file_ = nullptr;
        return false;
    }
    (void)xQueueReceive(event_queue_, &event_, pdMS_TO_TICKS(300));
    return true;
#else
    ESP_LOGE(TAG, "esp_audio_player not available; cannot play '%s'", path.c_str());
    fclose(file_); file_ = nullptr;
    return false;
#endif
}

void Mp3Player::pause() noexcept {
#if HAVE_ESP_AUDIO_PLAYER
    if (audio_player_get_state() == AUDIO_PLAYER_STATE_PLAYING) {
        expected_event_ = AUDIO_PLAYER_CALLBACK_EVENT_PAUSE;
        if (audio_player_pause() != ESP_OK) return;
        (void)xQueueReceive(event_queue_, &event_, pdMS_TO_TICKS(200));
    }
#endif
}

void Mp3Player::resume() noexcept {
#if HAVE_ESP_AUDIO_PLAYER
    if (audio_player_get_state() != AUDIO_PLAYER_STATE_PLAYING) {
        expected_event_ = AUDIO_PLAYER_CALLBACK_EVENT_PLAYING;
        if (audio_player_resume() != ESP_OK) return;
        (void)xQueueReceive(event_queue_, &event_, pdMS_TO_TICKS(200));
    }
#endif
}

void Mp3Player::stop() noexcept {
    // Best-effort stop: pause + close file
    pause();
#if HAVE_ESP_AUDIO_PLAYER
    (void)audio_player_stop();
#endif
    // FILE is closed by audio_player thread; just clear local pointer
    file_ = nullptr;
}

void Mp3Player::set_volume(uint8_t vol_0_100) noexcept {
    if (vol_0_100 > 100) vol_0_100 = 100;
    volume_.store(vol_0_100);
    Audio_Volume = vol_0_100;
}

bool Mp3Player::is_playing() const noexcept {
#if HAVE_ESP_AUDIO_PLAYER
    return audio_player_get_state() == AUDIO_PLAYER_STATE_PLAYING;
#else
    return false;
#endif
}
bool Mp3Player::is_paused()  const noexcept {
#if HAVE_ESP_AUDIO_PLAYER
    return audio_player_get_state() == AUDIO_PLAYER_STATE_PAUSE;
#else
    return false;
#endif
}

// ---- C API wrappers ----

extern "C" void Audio_Init(void) {
    if (!g_player) g_player = new Mp3Player();
    g_player->init(44100, 16, true);
}

extern "C" void Audio_Click_Init(void) {
    if (!g_player) g_player = new Mp3Player();
    if (!g_player->init_i2s_only(44100, 16, true)) {
        ESP_LOGE(TAG, "Audio_Click_Init: I2S init failed");
    }
}

extern "C" void Play_Click(void) {
    if (g_player) g_player->play_click();
}

extern "C" bool Play_Tone(uint16_t freq_hz, uint16_t dur_ms, uint8_t level) {
    return g_player && g_player->play_tone(freq_hz, dur_ms, level);
}

extern "C" void Play_Music(const char* directory, const char* fileName) {
    if (!g_player) Audio_Init();
    std::string path;
    if (directory && directory[0]) {
        path = directory;
        if (path.size() > 1 && path.back() != '/') path.push_back('/');
        if (fileName) path += fileName;
    } else if (fileName) {
        path = fileName;
    }
    g_player->play_file(path);
}

extern "C" void Music_pause(void)  { if (g_player) g_player->pause(); }
extern "C" void Music_resume(void) { if (g_player) g_player->resume(); }
extern "C" void Music_stop(void)   { if (g_player) g_player->stop(); }
extern "C" bool Music_is_playing(void) {
#if HAVE_ESP_AUDIO_PLAYER
    return audio_player_get_state() == AUDIO_PLAYER_STATE_PLAYING;
#else
    return false;
#endif
}
extern "C" void Volume_adjustment(uint8_t Vol) { if (g_player) g_player->set_volume(Vol); Audio_Volume = Vol; }
extern "C" bool Music_is_paused(void) { return g_player && g_player->is_paused(); }
extern "C" bool Music_finished(void)  { return g_player && g_player->finished_flag(); }
extern "C" bool Music_play_path(const char* path) {
    if (!g_player || !path) return false;
    return g_player->play_file(path);
}

} // namespace audio
