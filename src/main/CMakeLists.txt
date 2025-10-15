idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES power_key display touch ui
    PRIV_REQUIRES nvs_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
