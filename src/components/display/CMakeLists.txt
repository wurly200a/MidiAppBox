
idf_component_register(SRCS "display.cpp"
                      INCLUDE_DIRS "."
                      REQUIRES esp_lcd esp_lvgl_port driver esp_timer
                      PRIV_REQUIRES board)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
