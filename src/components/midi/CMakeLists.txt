idf_component_register(
    SRCS "midi.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver freertos esp_timer
    PRIV_REQUIRES board
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
