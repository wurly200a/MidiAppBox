idf_component_register(
    SRCS "touch.cpp"
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lvgl_port esp_driver_i2c esp_driver_gpio
    PRIV_REQUIRES board
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
