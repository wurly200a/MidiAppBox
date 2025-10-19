#include "sdcard.hpp"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include "board_pins.hpp"
#include <dirent.h>
#include <cstring>

namespace storage {

static const char* TAG_SD = "SDCARD";

bool SdCard::mount(const char* mount_point) {
    if (mounted_) return true;

    ESP_LOGI(TAG_SD, "Using SPI host=%d MOSI=%d MISO=%d SCLK=%d CS=%d",
             (int)SD_SPI_HOST, (int)PIN_SD_MOSI, (int)PIN_SD_MISO, (int)PIN_SD_SCLK, (int)PIN_SD_CS);

    // Enable weak pull-ups (some boards require them for reliable init)
    gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_SCLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_CS,   GPIO_PULLUP_ONLY);

    esp_err_t err = ESP_FAIL;

    // Try SDMMC host first if SDMMC pins are defined
#ifdef PIN_SDMMC_CLK
    do {
        ESP_LOGI(TAG_SD, "Trying SDMMC host: CLK=%d CMD=%d D0=%d",
                 (int)PIN_SDMMC_CLK, (int)PIN_SDMMC_CMD, (int)PIN_SDMMC_D0);

        // Pull-ups recommended for SDMMC lines
        gpio_set_pull_mode((gpio_num_t)PIN_SDMMC_CMD, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode((gpio_num_t)PIN_SDMMC_CLK, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode((gpio_num_t)PIN_SDMMC_D0,  GPIO_PULLUP_ONLY);
#ifdef PIN_SDMMC_D1
        if ((int)PIN_SDMMC_D1 >= 0) gpio_set_pull_mode((gpio_num_t)PIN_SDMMC_D1, GPIO_PULLUP_ONLY);
#endif
#ifdef PIN_SDMMC_D2
        if ((int)PIN_SDMMC_D2 >= 0) gpio_set_pull_mode((gpio_num_t)PIN_SDMMC_D2, GPIO_PULLUP_ONLY);
#endif
#ifdef PIN_SDMMC_D3
        if ((int)PIN_SDMMC_D3 >= 0) gpio_set_pull_mode((gpio_num_t)PIN_SDMMC_D3, GPIO_PULLUP_ONLY);
#endif

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = 20000; // 20 MHz for stability

        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.clk = (gpio_num_t)PIN_SDMMC_CLK;
        slot_config.cmd = (gpio_num_t)PIN_SDMMC_CMD;
        slot_config.d0  = (gpio_num_t)PIN_SDMMC_D0;
#ifdef PIN_SDMMC_D1
        if ((int)PIN_SDMMC_D1 >= 0) slot_config.d1  = (gpio_num_t)PIN_SDMMC_D1;
#endif
#ifdef PIN_SDMMC_D2
        if ((int)PIN_SDMMC_D2 >= 0) slot_config.d2  = (gpio_num_t)PIN_SDMMC_D2;
#endif
#ifdef PIN_SDMMC_D3
        if ((int)PIN_SDMMC_D3 >= 0) slot_config.d3  = (gpio_num_t)PIN_SDMMC_D3;
#endif

        // If only D0 is provided, force 1-bit mode
        // Determine bus width: default to 1-bit if not all D1-D3 provided
        host.flags = SDMMC_HOST_FLAG_1BIT;
#if defined(PIN_SDMMC_D1) && defined(PIN_SDMMC_D2) && defined(PIN_SDMMC_D3)
        if ((int)PIN_SDMMC_D1 >= 0 && (int)PIN_SDMMC_D2 >= 0 && (int)PIN_SDMMC_D3 >= 0) {
            host.flags &= ~SDMMC_HOST_FLAG_1BIT; // allow 4-bit
        }
#endif

        esp_vfs_fat_sdmmc_mount_config_t mount_config{};
        mount_config.format_if_mount_failed = false;
        mount_config.max_files = 8;
        mount_config.allocation_unit_size = 16 * 1024;

        sdmmc_card_t* card = nullptr;
        err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
        if (err == ESP_OK) {
            mount_point_ = mount_point;
            mounted_ = true;
            card_ = card;
            sdmmc_card_print_info(stdout, card);
            return true;
        }
        ESP_LOGW(TAG_SD, "SDMMC mount failed: %d, falling back to SDSPI", (int)err);
    } while (0);
#endif // PIN_SDMMC_CLK

    // Fallback to SDSPI host
    spi_bus_config_t bus_cfg{};
    bus_cfg.mosi_io_num = PIN_SD_MOSI;
    bus_cfg.miso_io_num = PIN_SD_MISO;
    bus_cfg.sclk_io_num = PIN_SD_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_SD, "spi_bus_initialize failed: %d", (int)err);
        return false;
    }

    sdmmc_host_t host_spi = SDSPI_HOST_DEFAULT();
    host_spi.slot = (spi_host_device_t)SD_SPI_HOST;
    host_spi.max_freq_khz = 12000; // 12 MHz

    sdspi_device_config_t slot_spi = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_spi.gpio_cs = PIN_SD_CS;
    slot_spi.host_id = (spi_host_device_t)SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg{};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 8;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    err = esp_vfs_fat_sdspi_mount(mount_point, &host_spi, &slot_spi, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SD, "esp_vfs_fat_sdspi_mount failed: %d", (int)err);
        return false;
    }
    mount_point_ = mount_point;
    mounted_ = true;
    card_ = card;
    sdmmc_card_print_info(stdout, card);
    return true;
}

void SdCard::unmount() {
    if (!mounted_) return;
    esp_vfs_fat_sdcard_unmount(mount_point_.c_str(), card_);
    spi_bus_free((spi_host_device_t)SD_SPI_HOST);
    mounted_ = false;
    card_ = nullptr;
}

std::vector<std::string> SdCard::list_dir(const char* path) {
    std::vector<std::string> out;
    DIR* dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG_SD, "opendir failed: %s", path);
        return out;
    }
    while (true) {
        errno = 0;
        dirent* ent = readdir(dir);
        if (!ent) break;
        const char* name = ent->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
        out.emplace_back(name);
    }
    closedir(dir);
    return out;
}

} // namespace storage
