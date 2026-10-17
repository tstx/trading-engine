cmake_minimum_required(VERSION 3.20)
project(trading_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENGINE_BUILD_TESTS "Build tests" ON)
set(ENGINE_SANITIZE "" CACHE STRING "Sanitizers, e.g. 'address,undefined' or 'thread'")

# Warnungen nur für eigene Targets (nicht für FetchContent-Abhängigkeiten).
add_library(engine_warnings INTERFACE)
target_compile_options(engine_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
if(ENGINE_SANITIZE)
    add_compile_options(-fsanitize=${ENGINE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${ENGINE_SANITIZE})
endif()

add_library(engine_core
    src/types.cpp
    src/symbol_table.cpp
    src/order_book.cpp
    src/matching_engine.cpp
    src/parser.cpp
    src/text_event_sink.cpp
)
target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core PRIVATE engine_warnings)

add_executable(trading_engine src/main.cpp)
target_link_libraries(trading_engine PRIVATE engine_core engine_warnings)

if(ENGINE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    file(GLOB ENGINE_TEST_SOURCES CONFIGURE_DEPENDS tests/*_test.cpp)
    add_executable(engine_tests ${ENGINE_TEST_SOURCES})
    target_include_directories(engine_tests PRIVATE tests)
    target_link_libraries(engine_tests PRIVATE engine_core engine_warnings GTest::gtest_main)
    gtest_discover_tests(engine_tests)

    # Replay-Tests: jedes tests/replay/<name>.in muss exakt <name>.expected ergeben (NFR-5).
    file(GLOB ENGINE_REPLAY_INPUTS CONFIGURE_DEPENDS tests/replay/*.in)
    foreach(input ${ENGINE_REPLAY_INPUTS})
        get_filename_component(name ${input} NAME_WE)
        add_test(NAME replay.${name}
            COMMAND ${CMAKE_COMMAND}
                -DEXE=$<TARGET_FILE:trading_engine>
                -DINPUT=${input}
                -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/${name}.expected
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/replay/${name}.out
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_replay.cmake)
    endforeach()
endif()
