
cmake_minimum_required(VERSION 3.16)
set(PROJECT_NAME "midi_app_box")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_DEFINITIONS "LV_COLOR_16_SWAP=1" APPEND)
project(${PROJECT_NAME})

# WAMR 2.4.0 は WAMR_ENABLE_MEMORY_PROFILING=y のとき RECORD_STACK_USAGE が
# -Werror=dangling-pointer に引っかかる(意図的にローカル変数のアドレスで
# スタック使用量を記録する実装)。当該コンポーネントのみ警告を抑制する。
idf_component_get_property(wamr_lib espressif__wasm-micro-runtime COMPONENT_LIB)
if(wamr_lib)
    target_compile_options(${wamr_lib} PRIVATE -Wno-dangling-pointer)
endif()
