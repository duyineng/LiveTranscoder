cmake_minimum_required(VERSION 3.20)

project(LiveTranscoder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

message(STATUS "CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "VCPKG_TARGET_TRIPLET = ${VCPKG_TARGET_TRIPLET}")

add_executable(LiveTranscoder
    src/main.cpp
    src/MediaProbe.cpp
    src/Remuxer.cpp
    src/Transcoder.cpp
)

target_include_directories(LiveTranscoder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(FFMPEG REQUIRED)

target_include_directories(LiveTranscoder PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(LiveTranscoder PRIVATE
    ${FFMPEG_LIBRARIES}
)

target_compile_options(LiveTranscoder PRIVATE 
    /utf-8
)
