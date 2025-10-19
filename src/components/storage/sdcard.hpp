#pragma once
#include <vector>
#include <string>
#include "sdmmc_cmd.h"

namespace storage {

class SdCard {
public:
    // Mount SD card over SPI. Returns true on success.
    bool mount(const char* mount_point = "/sdcard");
    // Unmount if mounted.
    void unmount();
    // List files in a directory (non-recursive). Returns empty on failure.
    std::vector<std::string> list_dir(const char* path = "/sdcard");

private:
    bool mounted_ = false;
    std::string mount_point_;
    sdmmc_card_t* card_ = nullptr;
};

} // namespace storage
