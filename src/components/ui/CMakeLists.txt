
idf_component_register(SRCS "ui.cpp"
                      INCLUDE_DIRS "."
                      REQUIRES lvgl esp_lvgl_port)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
