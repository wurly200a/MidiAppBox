idf_component_register(
    SRCS "seq.cpp" "clock_authority.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared/seq_core.c"
    INCLUDE_DIRS "."
    REQUIRES freertos esp_timer
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared"
    PRIV_REQUIRES midi
)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
