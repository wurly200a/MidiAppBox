idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES power_key display touch audio midi seq
    PRIV_REQUIRES nvs_flash wasm_runtime
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
