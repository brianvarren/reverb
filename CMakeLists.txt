cmake_minimum_required(VERSION 3.16)
project(reverb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED rtaudio)

add_executable(reverb src/main.cpp)

target_compile_options(reverb PRIVATE -O2 -Wall)
target_include_directories(reverb PRIVATE ${RTAUDIO_INCLUDE_DIRS})
target_link_libraries(reverb PRIVATE ${RTAUDIO_LIBRARIES})
target_link_directories(reverb PRIVATE ${RTAUDIO_LIBRARY_DIRS})
