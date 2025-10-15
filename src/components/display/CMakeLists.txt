
idf_component_register(SRCS "display.cpp"
                      INCLUDE_DIRS "."
                      REQUIRES esp_lcd esp_lvgl_port driver
                      PRIV_REQUIRES board)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
