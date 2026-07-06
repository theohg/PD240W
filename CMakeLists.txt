# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Default to a Debug build when none is specified (e.g. a bare `cmake -G Ninja ..`).
# The build type now matters: it drives DEBUG_BUILD (see target_compile_definitions
# below), so an unspecified/Release configure would silently drop LOG_DEBUG. The
# VS Code CMake Tools extension passes -DCMAKE_BUILD_TYPE explicitly, so this only
# affects command-line configures and never overrides an explicit choice.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(PD240W C CXX ASM)

# Set the external flash size to 16MB (128Mbit) for the RP2040.
# Single source of truth: drives the linker flash region (CMake var, below)
# AND the C macro (compile definition, in target_compile_definitions further down).
# Plain integer (no spaces) so it works cleanly as a -D flag.
set(PICO_FLASH_SIZE_BYTES "16777216")   # 16 MB = 128 Mbit

set(INA228_SUBMODULE_DIR "${CMAKE_CURRENT_LIST_DIR}/external/ina228_multiplatform")
set(INA228_FIRMWARE_VERSION "unknown")
set(TPS26750_SUBMODULE_DIR "${CMAKE_CURRENT_LIST_DIR}/external/tps26750_multiplatform")
set(TPS26750_FIRMWARE_VERSION "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${INA228_SUBMODULE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${INA228_SUBMODULE_DIR}" describe --tags --always --abbrev=0
        OUTPUT_VARIABLE INA228_FIRMWARE_VERSION
        RESULT_VARIABLE INA228_GIT_DESCRIBE_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT INA228_GIT_DESCRIBE_RESULT EQUAL 0 OR INA228_FIRMWARE_VERSION STREQUAL "")
        set(INA228_FIRMWARE_VERSION "unknown")
    endif()
endif()
if(GIT_FOUND AND EXISTS "${TPS26750_SUBMODULE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${TPS26750_SUBMODULE_DIR}" describe --tags --always --abbrev=0
        OUTPUT_VARIABLE TPS26750_FIRMWARE_VERSION
        RESULT_VARIABLE TPS26750_GIT_DESCRIBE_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT TPS26750_GIT_DESCRIBE_RESULT EQUAL 0 OR TPS26750_FIRMWARE_VERSION STREQUAL "")
        set(TPS26750_FIRMWARE_VERSION "unknown")
    endif()
endif()

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

# Project sources (our code + the two external driver submodules). Kept in a
# variable so warning flags can be scoped to exactly these, leaving the Pico
# SDK's own sources untouched.
set(PD240W_SOURCES
        # Core
        src/main.cpp
        src/hardware.cpp
        src/interrupts.cpp

        # Utils
        src/utils/tps_eeprom_loader.cpp
        src/utils/tps_patch_loader.cpp
        src/utils/tps26750_patch.c

        # Drivers
        src/drivers/buzzer/buzzer.cpp
        src/drivers/display/st7789.cpp
        external/tps26750_multiplatform/src/tps26750.cpp
        external/tps26750_multiplatform/src/tps26750_platform_i2c.cpp
        external/ina228_multiplatform/src/ina228.cpp
        external/ina228_multiplatform/src/ina228_platform_i2c.cpp
        src/drivers/rgb_led/sk6812.cpp
        src/drivers/input/rotary_enc.cpp
        src/drivers/input/adc_inputs.cpp
        src/drivers/gpio/gpio.cpp
        src/drivers/input/button.cpp

        # Logic (Phase 3)
        src/logic/state_machine.cpp
        src/logic/settings.cpp
        src/logic/safety.cpp
        src/logic/pd_manager.cpp
        src/logic/tps_eeprom_workflow.cpp
        src/logic/cc_controller.cpp

        # CLI (Serial command interface)
        src/cli/cli.cpp
        src/cli/cli_commands.cpp

        # UI
        src/ui/display_manager.cpp
)

add_executable(PD240W ${PD240W_SOURCES})

pico_set_program_name(PD240W "PD240W")
pico_set_program_version(PD240W "2.0.3")

# Enable warnings on our own sources only. Scoped via set_source_files_properties
# rather than target_compile_options so the Pico SDK's own sources (compiled into
# this same target) are not flagged. Enforces format-string (%lu/%ld) and
# init-order cleanliness. Append -Werror here once the tree stays clean.
set(PD240W_WARN_FLAGS -Wall -Wextra)
set_source_files_properties(${PD240W_SOURCES} PROPERTIES COMPILE_OPTIONS "${PD240W_WARN_FLAGS}")

# =============================================================================
# STDIO Configuration - Choose ONE output method
# =============================================================================
# Option 1: UART on GPIO 16 (TX) and GPIO 29 (RX) - for debug probe/FTDI
# Option 2: USB Serial - appears as /dev/ttyACM0 or COM port
#
# To switch: change the 1/0 values below and re-run CMake (not just ninja)
# =============================================================================
set(STDIO_UART 1)   # Set to 1 for UART output, 0 to disable
set(STDIO_USB  0)   # Set to 1 for USB serial output, 0 to disable

pico_enable_stdio_uart(PD240W ${STDIO_UART})
pico_enable_stdio_usb(PD240W ${STDIO_USB})

# Get build timestamp (updates on each build)
string(TIMESTAMP BUILD_TIMESTAMP "%b %d %Y" UTC)

# ADD THESE LINES TO SET YOUR CUSTOM PINS:
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
  PICO_DEFAULT_UART=0
  PICO_DEFAULT_UART_TX_PIN=16
  PICO_DEFAULT_UART_RX_PIN=29
  PICO_DEFAULT_UART_BAUD_RATE=115200
  # Make the C code (About screen, settings storage) see the real flash size.
  PICO_FLASH_SIZE_BYTES=${PICO_FLASH_SIZE_BYTES}
  # Build date from CMake (always updates on rebuild)
  BUILD_DATE="${BUILD_TIMESTAMP}"
    INA228_FW_VERSION="${INA228_FIRMWARE_VERSION}"
    TPS26750_FW_VERSION="${TPS26750_FIRMWARE_VERSION}"
  # Debug logging: enabled only for Debug builds. Release / MinSizeRel /
  # RelWithDebInfo compile LOG_DEBUG out (see src/utils/logging.h). Overrides the
  # header's fallback default of 1.
  DEBUG_BUILD=$<IF:$<CONFIG:Debug>,1,0>
)

# Add the standard library to the build
target_link_libraries(PD240W
        pico_stdlib
        hardware_i2c
        hardware_spi
        hardware_pio
        hardware_adc
        hardware_pwm
        hardware_flash
        hardware_sync)

# Add the standard include files to the build
target_include_directories(PD240W PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
    external/ina228_multiplatform/include
    external/tps26750_multiplatform/include
        src
        src/config
        src/drivers
        src/logic
        src/utils
        src/ui
)

pico_add_extra_outputs(PD240W)

# Post-build: display flash usage (find the size tool from the toolchain)
find_program(ARM_SIZE arm-none-eabi-size HINTS ${PICO_TOOLCHAIN_PATH}/bin)
if(ARM_SIZE)
    add_custom_command(TARGET PD240W POST_BUILD
        COMMAND ${ARM_SIZE} $<TARGET_FILE:PD240W>
        COMMENT "Flash/RAM usage:"
    )
endif()

pico_generate_pio_header(PD240W ${CMAKE_CURRENT_LIST_DIR}/src/drivers/rgb_led/sk6812.pio)