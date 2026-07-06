cmake_minimum_required(VERSION 3.20)
project(PD240W_Tests CXX)

# Host-only unit-test build. Does NOT use the Pico SDK — it compiles the pure,
# hardware-free logic headers directly and exercises them with Snitch.
# See notes/UNIT_TEST.md for the full plan; this is the logic-layer slice.

# Refuse in-source builds. Always configure into a subdirectory (test/build) so
# generated CMake files never pollute the source tree.
if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR
        "In-source build blocked. Build from a subdirectory instead:\n"
        "  cd test && cmake -G Ninja -B build && cmake --build build")
endif()

set(CMAKE_CXX_STANDARD 20)          # Snitch requires C++20 (the tested headers are C++17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Options
# ============================================================================
option(ENABLE_SANITIZERS "Enable ASan + UBSan" ON)
option(ENABLE_COVERAGE   "Enable gcov coverage" ON)

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

if(ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# ============================================================================
# Snitch test framework (fetched at configure time)
# ============================================================================
include(FetchContent)
FetchContent_Declare(snitch
    GIT_REPOSITORY https://github.com/snitch-org/snitch.git
    GIT_TAG        v1.2.5
)
FetchContent_MakeAvailable(snitch)

# ============================================================================
# Test executable
# ============================================================================
add_executable(pd240w_tests
    test_pd_diagnostics.cpp
    test_settings_storage.cpp
    test_thermal_monitor.cpp
    test_programmable_tuning.cpp
)

# The tested logic headers assume a "native" (non-embedded) TPS26750 platform so
# the multiplatform driver's data types compile without an MCU HAL.
target_compile_definitions(pd240w_tests PRIVATE
    UNIT_TEST_BUILD
    TPS26750_PLATFORM_NATIVE
)

target_include_directories(pd240w_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/logic
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/tps26750_multiplatform/include
)

# -Wno-sign-compare: Snitch decomposes CHECK(a == b) into a generic comparison
# template, so comparing a uint32_t return against a plain int literal trips
# -Wsign-compare inside the framework, not in real logic. Keep the rest of -Wextra.
target_compile_options(pd240w_tests PRIVATE -Wall -Wextra -Wno-sign-compare)
target_link_libraries(pd240w_tests PRIVATE snitch::snitch)

# ============================================================================
# CTest
# ============================================================================
enable_testing()
add_test(NAME unit_tests COMMAND pd240w_tests)

# ============================================================================
# Coverage report target
# ============================================================================
if(ENABLE_COVERAGE)
    find_program(LCOV lcov)
    find_program(GENHTML genhtml)
    if(LCOV AND GENHTML)
        add_custom_target(coverage
            COMMAND ${CMAKE_CURRENT_BINARY_DIR}/pd240w_tests
            COMMAND ${LCOV} --directory . --capture --output-file coverage.info
                    --ignore-errors mismatch,unsupported,format
            COMMAND ${LCOV} --remove coverage.info '*/test/*' '*/_deps/*' '/usr/*'
                    --ignore-errors unused --output-file coverage_filtered.info
            COMMAND ${GENHTML} coverage_filtered.info
                    --output-directory ${CMAKE_CURRENT_BINARY_DIR}/coverage_report
            DEPENDS pd240w_tests
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating coverage report -> test/build/coverage_report/index.html"
        )
    else()
        # Still define the target so `ninja coverage` gives a helpful message
        # instead of "unknown target". lcov/genhtml ship via `brew install lcov`.
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E false
            COMMENT "Coverage needs lcov + genhtml. Install with: brew install lcov"
        )
    endif()
endif()
