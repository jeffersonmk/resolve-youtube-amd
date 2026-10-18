cmake_minimum_required(VERSION 3.20)
project(youtube_amd_plugin CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_subdirectory(${SRC_DIR}/wrapper wrapper)

add_library(${PROJECT_NAME} SHARED
        ${SRC_DIR}/plugin.cpp
        ${SRC_DIR}/video_encoder.cpp
        ${SRC_DIR}/audio_encoder.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${SRC_DIR} ${SRC_DIR}/include ${SRC_DIR}/wrapper)
target_compile_definitions(${PROJECT_NAME} PRIVATE __STDC_CONSTANT_MACROS)
target_compile_options(${PROJECT_NAME} PRIVATE -fPIC -Wall -Wno-multichar -Wno-unused-variable)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--no-undefined -Wl,-Bsymbolic)
target_link_libraries(${PROJECT_NAME} PRIVATE host_plugin PkgConfig::FFMPEG PkgConfig::LIBVA)

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".dvcp")
