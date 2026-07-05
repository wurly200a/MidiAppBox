idf_component_register(
    SRCS "wasm_runtime.cpp" "hostapi.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../wasm-apps/hello/hello.wasm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../wasm-apps/demo/demo.wasm"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared"
    PRIV_REQUIRES esp_timer audio
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
