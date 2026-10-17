cmake_minimum_required(VERSION 3.20)
project(trading_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENGINE_BUILD_TESTS "Build tests" ON)
option(ENGINE_BUILD_BENCH "Build benchmarks" ON)
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
    src/event.cpp
    src/pipeline.cpp
)
find_package(Threads REQUIRED)
target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core PUBLIC Threads::Threads PRIVATE engine_warnings)

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

    # Replay-Tests: jedes tests/replay/<name>.in muss exakt <name>.expected ergeben (NFR-5),
    # single-threaded und über die Pipeline (--threaded).
    file(GLOB ENGINE_REPLAY_INPUTS CONFIGURE_DEPENDS tests/replay/*.in)
    foreach(input ${ENGINE_REPLAY_INPUTS})
        get_filename_component(name ${input} NAME_WE)
        foreach(mode single threaded)
            if(mode STREQUAL "threaded")
                set(mode_args --threaded)
            else()
                set(mode_args "")
            endif()
            add_test(NAME replay.${mode}.${name}
                COMMAND ${CMAKE_COMMAND}
                    -DEXE=$<TARGET_FILE:trading_engine>
                    "-DARGS=${mode_args}"
                    -DINPUT=${input}
                    -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/${name}.expected
                    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/replay/${mode}/${name}.out
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_replay.cmake)
        endforeach()
    endforeach()
endif()

if(ENGINE_BUILD_BENCH)
    include(FetchContent)
    FetchContent_Declare(benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    file(GLOB ENGINE_BENCH_SOURCES CONFIGURE_DEPENDS bench/*.cpp)
    add_executable(engine_bench ${ENGINE_BENCH_SOURCES})
    target_include_directories(engine_bench PRIVATE bench)
    target_link_libraries(engine_bench PRIVATE engine_core engine_warnings benchmark::benchmark)
endif()
