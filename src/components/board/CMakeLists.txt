idf_component_register(
    SRCS "board_pins.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
