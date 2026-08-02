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

# ── verb_tui: realtime tuning bench (ncurses + miniaudio) ─────────────────────
# Optional target. If ncurses is missing the rest of the project still builds;
# install it with:  sudo apt install libncursesw5-dev
set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE    TRUE)
find_package(Curses QUIET)
if(NOT CURSES_FOUND)
    set(CURSES_NEED_WIDE FALSE)
    find_package(Curses QUIET)
endif()

if(CURSES_FOUND)
    # miniaudio is a single public-domain header; fetched once, gitignored.
    set(MINIAUDIO ${CMAKE_SOURCE_DIR}/tui/miniaudio.h)
    if(NOT EXISTS ${MINIAUDIO})
        message(STATUS "fetching miniaudio.h")
        file(DOWNLOAD
             https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
             ${MINIAUDIO} TLS_VERIFY ON STATUS ma_dl)
        list(GET ma_dl 0 ma_code)
        if(NOT ma_code EQUAL 0)
            file(REMOVE ${MINIAUDIO})
            message(FATAL_ERROR "could not download miniaudio.h — fetch it manually into tui/")
        endif()
    endif()

    find_package(Threads REQUIRED)
    add_executable(verb_tui tui/main.cpp)
    target_link_libraries(verb_tui PRIVATE
        verb_core ${CURSES_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS} m)
    target_include_directories(verb_tui PRIVATE
        ${CMAKE_SOURCE_DIR}/tui ${CMAKE_SOURCE_DIR}/host ${CURSES_INCLUDE_DIRS})
    target_compile_options(verb_tui PRIVATE -O2 -Wall -Wextra)
    if(CURSES_HAVE_NCURSESW_NCURSES_H OR CURSES_NEED_WIDE)
        target_compile_definitions(verb_tui PRIVATE VERB_TUI_WIDE)
    endif()
else()
    message(STATUS "ncurses not found — skipping verb_tui")
endif()
