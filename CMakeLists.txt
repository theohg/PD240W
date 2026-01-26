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
        src/main.cpp
        src/hardware.cpp
        src/interrupts.cpp
        src/drivers/buzzer/buzzer.cpp
        src/drivers/display/st7789.cpp
        src/drivers/power/tps26750/tps26750.cpp
        src/drivers/power/ina228/ina228.cpp
        src/drivers/rgb_led/sk6812.cpp
        src/drivers/input/rotary_enc.cpp
        src/drivers/input/adc_inputs.cpp
        src/drivers/gpio/gpio.cpp
        src/drivers/input/button.cpp
        src/ui/display_manager.cpp )

pico_set_program_name(PD240W "PD240W")
pico_set_program_version(PD240W "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(PD240W 1)
pico_enable_stdio_usb(PD240W 0)

# ADD THESE LINES TO SET YOUR CUSTOM PINS:
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
  PICO_DEFAULT_UART=0
  PICO_DEFAULT_UART_TX_PIN=16
  PICO_DEFAULT_UART_RX_PIN=29
  PICO_DEFAULT_UART_BAUD_RATE=115200
)

# Add the standard library to the build
target_link_libraries(PD240W
        pico_stdlib
        hardware_i2c
        hardware_spi
        hardware_pio
        hardware_adc
        hardware_pwm)

# Add the standard include files to the build
target_include_directories(PD240W PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        src
        src/drivers
        src/hal
        src/ui
)

pico_add_extra_outputs(PD240W)

pico_generate_pio_header(PD240W ${CMAKE_CURRENT_LIST_DIR}/src/drivers/rgb_led/sk6812.pio)