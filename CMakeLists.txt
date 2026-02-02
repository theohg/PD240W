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
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(PD240W C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(PD240W
        # Core
        src/main.cpp
        src/hardware.cpp
        src/interrupts.cpp

        # Utils
        src/utils/eeprom_loader.cpp
        src/utils/tps26750_patch.c

        # Drivers
        src/drivers/buzzer/buzzer.cpp
        src/drivers/display/st7789.cpp
        src/drivers/power/tps26750/tps26750.cpp
        src/drivers/power/ina228/ina228.cpp
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
        src/logic/eeprom_workflow.cpp

        # UI
        src/ui/display_manager.cpp
)

pico_set_program_name(PD240W "PD240W")
pico_set_program_version(PD240W "0.1")

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
  # Build date from CMake (always updates on rebuild)
  BUILD_DATE="${BUILD_TIMESTAMP}"
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