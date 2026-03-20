cmake_minimum_required(VERSION 3.20)
project(slang-cdc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(SLANG_CDC_BUILD_TESTS "Build test suite" ON)
option(SLANG_CDC_USE_SYSTEM_SLANG "Use system-installed slang instead of FetchContent" OFF)

# --- Dependencies ---
if(SLANG_CDC_USE_SYSTEM_SLANG)
    find_package(slang REQUIRED)
else()
    include(cmake/FetchSlang.cmake)
endif()

# --- Library (core analysis passes) ---
add_library(slang-cdc-lib STATIC
    src/clock_tree.cpp
    src/sdc_parser.cpp
    src/ff_classifier.cpp
    src/connectivity.cpp
    src/crossing_detector.cpp
    src/sync_verifier.cpp
    src/report_generator.cpp
    src/waiver.cpp
    src/clock_yaml_parser.cpp
)

target_include_directories(slang-cdc-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(slang-cdc-lib PUBLIC slang::slang)

# --- Executable ---
add_executable(slang-cdc src/main.cpp)
target_link_libraries(slang-cdc PRIVATE slang-cdc-lib)

# --- Tests ---
if(SLANG_CDC_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(slang-cdc-tests
        tests/test_sdc_parser.cpp
        tests/test_clock_database.cpp
        tests/test_clock_tree.cpp
        tests/test_ff_classifier.cpp
        tests/test_connectivity_and_crossing.cpp
        tests/test_sync_verifier.cpp
        tests/test_report_generator.cpp
        tests/test_edge_cases.cpp
        tests/test_phase2_hierarchy.cpp
        tests/test_phase3_advanced.cpp
        tests/test_phase4_usability.cpp
        tests/test_issue_fixes.cpp
        tests/test_remaining_features.cpp
        tests/test_sync_patterns.cpp
        tests/test_clock_yaml.cpp
    )
    target_link_libraries(slang-cdc-tests PRIVATE slang-cdc-lib Catch2::Catch2WithMain)
    add_test(NAME slang-cdc-tests COMMAND slang-cdc-tests)
endif()

# --- Install ---
install(TARGETS slang-cdc DESTINATION bin)
