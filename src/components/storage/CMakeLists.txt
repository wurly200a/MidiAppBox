idf_component_register(SRCS "sdcard.cpp"
                      INCLUDE_DIRS "."
                      REQUIRES driver fatfs sdmmc vfs board)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
