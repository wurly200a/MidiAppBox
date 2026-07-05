cmake_minimum_required(VERSION 3.16)
project(midibox_linux_host C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- WAMR (実機と同一バージョンの vmlib を FetchContent で取得) ----
include(FetchContent)
FetchContent_Declare(wamr
    GIT_REPOSITORY https://github.com/bytecodealliance/wasm-micro-runtime.git
    GIT_TAG        WAMR-2.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(wamr)
if(NOT wamr_POPULATED)
    FetchContent_Populate(wamr)
endif()
set(WAMR_ROOT_DIR ${wamr_SOURCE_DIR})

# 実機側の構成に合わせる: fast interpreter + libc builtin のみ
set(WAMR_BUILD_PLATFORM "linux")
if(NOT DEFINED WAMR_BUILD_TARGET)
    set(WAMR_BUILD_TARGET "X86_64")
endif()
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_SHARED_MEMORY 0)

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_link_libraries(vmlib PUBLIC pthread m)

# ---- SDL2 ----
find_package(SDL2 REQUIRED)

# ---- host executable ----
add_executable(midibox_host main.c hostapi_sdl.c)
target_include_directories(midibox_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)
target_link_libraries(midibox_host PRIVATE vmlib SDL2::SDL2)
