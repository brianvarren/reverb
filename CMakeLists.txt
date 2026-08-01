cmake_minimum_required(VERSION 3.16)
project(verb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── verb_core: portable DSP — zero platform dependencies ──────────────────────
add_library(verb_core INTERFACE)
target_include_directories(verb_core INTERFACE ${CMAKE_SOURCE_DIR}/core)

# ── verb_cli: WAV-in / WAV-out host harness ───────────────────────────────────
add_executable(verb_cli host/main.cpp)
target_link_libraries(verb_cli PRIVATE verb_core)
target_include_directories(verb_cli PRIVATE ${CMAKE_SOURCE_DIR}/host)
target_compile_options(verb_cli PRIVATE -O2 -Wall -Wextra)

# ── verb_tests: doctest suite ─────────────────────────────────────────────────
add_executable(verb_tests tests/test_main.cpp)
target_link_libraries(verb_tests PRIVATE verb_core)
target_include_directories(verb_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(verb_tests PRIVATE -O2 -Wall -Wextra)
