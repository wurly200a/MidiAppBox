
idf_component_register(SRCS "power_key.cpp"
                      INCLUDE_DIRS "."
                      REQUIRES driver freertos
                      PRIV_REQUIRES board)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
