
cmake_minimum_required(VERSION 3.16)
set(PROJECT_NAME "midi_app_box")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_DEFINITIONS "LV_COLOR_16_SWAP=1" APPEND)
project(${PROJECT_NAME})
