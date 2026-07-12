idf_component_register(
    SRCS "wasm_runtime.cpp" "hostapi.cpp" "launcher.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../wasm-apps/hello/hello.wasm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../wasm-apps/demo/demo.wasm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../wasm-apps/bars/bars.wasm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../wasm-apps/bench/bench.wasm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../wasm-apps/touch_demo/touch_demo.wasm"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../shared"
    PRIV_REQUIRES esp_timer audio storage
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
