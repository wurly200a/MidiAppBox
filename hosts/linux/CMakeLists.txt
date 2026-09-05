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

# ---- SDL2_ttf(任意。あればアンチエイリアスのテキスト描画、無ければ font8x8) ----
find_package(SDL2_ttf QUIET)
if(NOT SDL2_ttf_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2_TTF_PC QUIET SDL2_ttf)
    endif()
endif()

# ---- SDL2_mixer(Phase 6B: MP3 再生。無ければ audio_play が常に ERROR) ----
find_package(SDL2_mixer QUIET)
if(NOT SDL2_mixer_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2_MIXER_PC QUIET SDL2_mixer)
    endif()
endif()

# ---- ALSA(任意。Phase 8b: MIDI OUT を実ポート(例: UM-ONE)へ送信する。
# 無ければ hostapi_midi_send は送信バイト列を stderr にログ出力するのみ) ----
find_package(ALSA QUIET)
if(NOT ALSA_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA_PC QUIET alsa)
    endif()
endif()

# ---- host executable ----
add_executable(midibox_host main.c hostapi_sdl.c hostapi_midi.c hostapi_seq.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/seq_core.c)
target_include_directories(midibox_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)
target_link_libraries(midibox_host PRIVATE vmlib SDL2::SDL2)

if(ALSA_FOUND)
    target_compile_definitions(midibox_host PRIVATE HAVE_ALSA)
    target_include_directories(midibox_host PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(midibox_host PRIVATE ${ALSA_LIBRARIES})
    message(STATUS "ALSA: found (MIDI OUT sent via ALSA sequencer)")
elseif(ALSA_PC_FOUND)
    target_compile_definitions(midibox_host PRIVATE HAVE_ALSA)
    target_include_directories(midibox_host PRIVATE ${ALSA_PC_INCLUDE_DIRS})
    target_link_libraries(midibox_host PRIVATE ${ALSA_PC_LIBRARIES})
    message(STATUS "ALSA: found via pkg-config (MIDI OUT sent via ALSA sequencer)")
else()
    message(STATUS "ALSA: not found (hostapi_midi_send will log to stderr only)")
endif()

if(SDL2_ttf_FOUND)
    target_compile_definitions(midibox_host PRIVATE HAVE_SDL_TTF)
    target_link_libraries(midibox_host PRIVATE SDL2_ttf::SDL2_ttf)
    message(STATUS "SDL2_ttf: found (antialiased text enabled)")
elseif(SDL2_TTF_PC_FOUND)
    target_compile_definitions(midibox_host PRIVATE HAVE_SDL_TTF)
    target_include_directories(midibox_host PRIVATE ${SDL2_TTF_PC_INCLUDE_DIRS})
    target_link_libraries(midibox_host PRIVATE ${SDL2_TTF_PC_LIBRARIES})
    message(STATUS "SDL2_ttf: found via pkg-config (antialiased text enabled)")
else()
    message(STATUS "SDL2_ttf: not found (falling back to font8x8)")
endif()

if(SDL2_mixer_FOUND)
    target_compile_definitions(midibox_host PRIVATE HAVE_SDL_MIXER)
    target_link_libraries(midibox_host PRIVATE SDL2_mixer::SDL2_mixer)
    message(STATUS "SDL2_mixer: found (MP3 playback enabled)")
elseif(SDL2_MIXER_PC_FOUND)
    target_compile_definitions(midibox_host PRIVATE HAVE_SDL_MIXER)
    target_include_directories(midibox_host PRIVATE ${SDL2_MIXER_PC_INCLUDE_DIRS})
    target_link_libraries(midibox_host PRIVATE ${SDL2_MIXER_PC_LIBRARIES})
    message(STATUS "SDL2_mixer: found via pkg-config (MP3 playback enabled)")
else()
    message(STATUS "SDL2_mixer: not found (hostapi_audio_play will fail)")
endif()
